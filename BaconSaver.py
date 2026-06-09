import fnmatch
import re
import subprocess
import threading
import time
from pathlib import Path
from typing import Callable

from watchdog.events import FileSystemEventHandler
from watchdog.observers import Observer


APP_DIR = Path(__file__).parent

# Always ignored — not user-removable, required for correct operation
ALWAYS_IGNORED = ['.git']

IGNORE_PRESETS: dict[str, list[str]] = {
    'General': [
        '.svn', 'x64',
        '*~', '*.TMP',
    ],
    'C++': [
        '.vs', 'x64', 'Debug', 'Release',
        '*.suo', '*.user', '*.sdf', '*.opensdf',
        '*.dll', '*.lib',
    ],
    'Python': [
        '__pycache__',
        '.mypy_cache', '.pytest_cache',
        '*.egg-info',
        '.venv', 'venv',
    ],
}


# ---------------------------------------------------------------------------
# IgnoreFilter
# ---------------------------------------------------------------------------

class IgnoreFilter:
    """Filters paths using .gitignore-style patterns loaded from a file.

    A pattern without / is matched against each individual path component.
    A pattern containing / is matched against the full relative path.
    Standard fnmatch wildcards: * ? [seq]
    """

    def __init__(self, pattern_file: Path) -> None:
        self._file = pattern_file
        self._component_patterns: list[str] = []
        self._path_patterns: list[str] = []
        self.reload()

    @property
    def file(self) -> Path:
        return self._file

    @property
    def patterns(self) -> list[str]:
        """Return the active patterns (no comments or blanks)."""
        return list(self._component_patterns) + list(self._path_patterns)

    def set_patterns(self, patterns: list[str]) -> None:
        """Overwrite the ignore file with new patterns and reload."""
        header = (
            '# BaconSaver ignore patterns\n'
            '# Pattern without / matches any path component; with / matches full path.\n'
            '# Wildcards: * ? [seq]   Comments: lines starting with #\n\n'
        )
        self._file.write_text(header + '\n'.join(patterns) + '\n')
        self.reload()

    def reload(self) -> None:
        self._component_patterns.clear()
        self._path_patterns.clear()
        if not self._file.exists():
            return
        for line in self._file.read_text().splitlines():
            p = line.strip()
            if not p or p.startswith('#'):
                continue
            if '/' in p.rstrip('/'):
                self._path_patterns.append(p.rstrip('/'))
            else:
                self._component_patterns.append(p)

    def is_ignored(self, rel_path: str) -> bool:
        parts = Path(rel_path).parts
        # Safety invariant: always skip version control internals
        for always in ALWAYS_IGNORED:
            if always in parts:
                return True
        for pat in self._component_patterns:
            for part in parts:
                if fnmatch.fnmatch(part, pat):
                    return True
        norm = rel_path.replace('\\', '/')
        for pat in self._path_patterns:
            if fnmatch.fnmatch(norm, pat):
                return True
        return False

    def as_git_exclude(self) -> str:
        always = '\n'.join(ALWAYS_IGNORED) + '\n'
        if self._file.exists():
            return always + self._file.read_text()
        return always


# ---------------------------------------------------------------------------
# Git helpers
# ---------------------------------------------------------------------------

def git_version() -> tuple[int, ...]:
    r = subprocess.run(['git', '--version'], capture_output=True, text=True, timeout=10.0)
    parts = r.stdout.strip().split()
    if len(parts) >= 3:
        ver = parts[2]
        nums = []
        for p in ver.split('.'):
            if p.isdigit():
                nums.append(int(p))
            else:
                break
        return tuple(nums) if nums else (0,)
    return (0,)


def shadow_name(watch_path: str) -> str:
    return re.sub(r'[^a-zA-Z0-9]', '_', watch_path).strip('_')


def _cleanup_stale_lock(git_dir: Path, log: Callable[[str], None]) -> None:
    lock = git_dir / 'index.lock'
    if not lock.exists():
        return
    age = time.time() - lock.stat().st_mtime
    if age > 30.0:
        lock.unlink()
        log('Removed stale index.lock')


def _git(args: list[str], git_dir: Path, work_tree: Path | None = None,
         check: bool = True, timeout: float = 30.0) -> subprocess.CompletedProcess:
    cmd = ['git', f'--git-dir={git_dir}']
    if work_tree is not None:
        cmd.append(f'--work-tree={work_tree}')
    cmd += args
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f'git {args[0]} timed out after {timeout}s (network drive down?)')
    if check and r.returncode != 0:
        # For 'add' commands, ignore errors caused by .git folders or stale locks
        if args[0] == 'add' and ('.git' in r.stderr or 'index.lock' in r.stderr or 'Permission denied' in r.stderr):
            # Still log but don't crash
            log_fn = getattr(_git, '_log', print)
            log_fn(f"Git add warning (ignored): {r.stderr.strip()}")
            return r
        raise RuntimeError(f'git {args[0]} failed:\n{r.stderr}')
    return r


def _apply_perf_config(git_dir: Path, log: Callable[[str], None]) -> None:
    """Set git config for large-repo performance.  Idempotent — safe to call on
    every startup.  fsmonitor is only enabled on Git 2.37+."""
    _git(['config', 'feature.manyFiles', 'true'], git_dir)
    _git(['config', 'index.version', '4'], git_dir)
    _git(['config', 'core.untrackedCache', 'true'], git_dir)
    ver = git_version()
    if ver >= (2, 37):
        _git(['config', 'core.fsmonitor', 'true'], git_dir)
        log('Performance: large-repo settings enabled (including fsmonitor)')
    else:
        log('Performance: large-repo settings enabled (fsmonitor skipped — Git '
            + '.'.join(str(v) for v in ver) + ' < 2.37)')


def _sync_git_exclude(shadow: Path, ignore: IgnoreFilter) -> None:
    exclude = shadow / '.git' / 'info' / 'exclude'
    exclude.parent.mkdir(exist_ok=True)
    exclude.write_text(ignore.as_git_exclude())


def _init_shadow_repo(watch: Path, shadow: Path, ignore: IgnoreFilter,
                      log: Callable[[str], None]) -> None:
    git_dir = shadow / '.git'
    _cleanup_stale_lock(git_dir, log)
    if git_dir.exists():
        _sync_git_exclude(shadow, ignore)
        _apply_perf_config(git_dir, log)
        # Exclude any .git directory (top-level or deeper) from being added
        _git(['add', '-A', '--', ':(exclude,top).git', ':(exclude)*/.git'], git_dir, watch)
        r = _git(['status', '--porcelain'], git_dir, watch, check=False)
        if r.stdout.strip():
            lines = r.stdout.strip().splitlines()
            log(f'Catching up {len(lines)} file(s) changed while offline...')
            for line in lines:
                log(f'  {line.strip()}')
            ts = time.strftime('%Y-%m-%d %H:%M:%S')
            _git(['commit', '-m', f'BaconSaver {ts} (catch-up)'], git_dir, watch)
            log('Catch-up commit done.')
        return

    # Init inside the shadow directory — never touches the watched directory at all.
    # The watched dir may already have its own .git which we must not disturb.
    shadow.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(['git', 'init', str(shadow)], capture_output=True, text=True, timeout=30.0)
    if r.returncode != 0:
        raise RuntimeError(f'git init failed:\n{r.stderr}')

    _git(['config', 'user.name', 'BaconSaver'], git_dir)
    _git(['config', 'user.email', 'baconsaver@local'], git_dir)
    _git(['config', 'core.autocrlf', 'false'], git_dir)
    _git(['config', 'core.worktree', str(watch)], git_dir)
    _apply_perf_config(git_dir, log)
    _sync_git_exclude(shadow, ignore)

    log(f'Initialized shadow repo: {git_dir}')
    log('Taking initial snapshot...')
    # Exclude any .git directory (top-level or deeper) from being added
    _git(['add', '-A', '--', ':(exclude,top).git', ':(exclude)*/.git'], git_dir, watch)
    r = _git(['status', '--porcelain'], git_dir, watch, check=False)
    if r.stdout.strip():
        _git(['commit', '-m', 'BaconSaver: initial snapshot'], git_dir, watch)
        log('Initial snapshot committed.')
    else:
        log('Nothing to snapshot.')


# ---------------------------------------------------------------------------
# History queries — read-only, used by restore UI
# ---------------------------------------------------------------------------

def get_commit_log(git_dir: Path, work_tree: Path) -> list[dict]:
    """Return list of {'hash', 'timestamp', 'message'} newest first."""
    r = _git(['log', '--format=%H\t%ai\t%s'], git_dir, work_tree, check=False)
    commits = []
    for line in r.stdout.strip().splitlines():
        if not line.strip():
            continue
        parts = line.split('\t', 2)
        if len(parts) == 3:
            commits.append({
                'hash': parts[0],
                'timestamp': parts[1],
                'message': parts[2],
            })
    return commits


def get_commit_files(git_dir: Path, work_tree: Path, commit_hash: str) -> list[dict]:
    """Return files changed in a commit: list of {'status', 'path'}.
    Status: A=added, M=modified, D=deleted."""
    r = _git(['diff-tree', '--no-commit-id', '-r', '--name-status', commit_hash],
             git_dir, work_tree, check=False)
    files = []
    for line in r.stdout.strip().splitlines():
        if '\t' in line:
            status, path = line.split('\t', 1)
            files.append({'status': status.strip(), 'path': path.strip()})
    return files


def get_file_at_commit(git_dir: Path, commit_hash: str, file_path: str) -> bytes:
    """Return raw file content at a specific commit."""
    r = subprocess.run(
        ['git', f'--git-dir={git_dir}', 'show', f'{commit_hash}:{file_path}'],
        capture_output=True, timeout=30.0
    )
    if r.returncode != 0:
        raise RuntimeError(f'Could not retrieve {file_path} at {commit_hash}')
    return r.stdout


def get_full_tree_at_commit(git_dir: Path, commit_hash: str) -> list[str]:
    """Return list of all file paths in the tree at a commit."""
    r = _git(['ls-tree', '-r', '--name-only', commit_hash], git_dir, check=False)
    return [line.strip() for line in r.stdout.strip().splitlines() if line.strip()]


def get_diff_for_commit(git_dir: Path, work_tree: Path, commit_hash: str,
                        file_path: str | None = None) -> str:
    """Return unified diff for a commit (vs its parent). Optionally filter to one file."""
    args = ['diff', f'{commit_hash}~1', commit_hash, '--']
    if file_path:
        args.append(file_path)
    r = _git(args, git_dir, work_tree, check=False)
    return r.stdout


def export_files(git_dir: Path, commit_hash: str, file_paths: list[str],
                 dest_dir: Path) -> list[str]:
    """Export specific files from a commit to dest_dir, preserving directory structure.
    Returns list of exported file paths."""
    exported = []
    for fp in file_paths:
        try:
            content = get_file_at_commit(git_dir, commit_hash, fp)
        except RuntimeError:
            continue
        out = dest_dir / fp
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(content)
        exported.append(fp)
    return exported


# ---------------------------------------------------------------------------
# Watchdog handler
# ---------------------------------------------------------------------------

class _Handler(FileSystemEventHandler):
    DEBOUNCE = 3.0

    def __init__(self, watch: Path, shadow: Path, ignore: IgnoreFilter,
                 log: Callable[[str], None]) -> None:
        self.watch = watch
        self.shadow = shadow
        self.git_dir = shadow / '.git'
        self._ignore = ignore
        self._log = log
        self._pending: dict[str, str] = {}
        self._lock = threading.Lock()
        self._timer: threading.Timer | None = None
        self._committing = False

    def _contains_git(self, path: str) -> bool:
        return '.git' in Path(path).parts

    def _schedule(self) -> None:
        if self._timer:
            self._timer.cancel()
        self._timer = threading.Timer(self.DEBOUNCE, self._commit)
        self._timer.daemon = True
        self._timer.start()

    def _commit(self) -> None:
        if self._committing:
            self._schedule()
            return
        self._committing = True
        try:
            with self._lock:
                pending = dict(self._pending)
                self._pending.clear()

            if not pending:
                return

            self._log(f'{len(pending)} change(s) pending, committing...')
            try:
                _cleanup_stale_lock(self.git_dir, self._log)
                # Exclude any .git directory (top-level or deeper) from being added
                _git(['add', '-A', '--', ':(exclude,top).git', ':(exclude)*/.git'], self.git_dir, self.watch)
                r = _git(['status', '--porcelain'], self.git_dir, self.watch, check=False)
                if r.stdout.strip():
                    ts = time.strftime('%Y-%m-%d %H:%M:%S')
                    for line in r.stdout.strip().splitlines():
                        self._log(f'  {line.strip()}')
                    _git(['commit', '-m', f'BaconSaver {ts}'], self.git_dir, self.watch)
                    n = len(r.stdout.strip().splitlines())
                    self._log(f'[{ts}] Committed {n} file(s).')
                else:
                    self._log('Nothing to commit (files may have been deleted)')
            except RuntimeError as e:
                self._log(f'Commit failed: {e}')
                # If the error mentions .git, it's likely harmless; continue
                if '.git' in str(e):
                    self._log('Note: This error was caused by a .git folder and was ignored.')
        finally:
            self._committing = False

    def _record(self, action: str, abs_path: str) -> None:
        p = Path(abs_path).resolve()
        if p.is_relative_to(self.shadow):
            return
        try:
            rel = str(p.relative_to(self.watch))
            # Skip any path that contains a .git component
            if '.git' in Path(rel).parts:
                return
        except ValueError:
            return
        if self._ignore.is_ignored(rel):
            return
        with self._lock:
            self._pending[rel] = action
        self._schedule()

    def on_created(self, event):
        if self._contains_git(event.src_path):
            return
        if not event.is_directory:
            self._record('changed', event.src_path)

    def on_modified(self, event):
        if self._contains_git(event.src_path):
            return
        if not event.is_directory:
            self._record('changed', event.src_path)

    def on_deleted(self, event):
        if self._contains_git(event.src_path):
            return
        if not event.is_directory:
            self._record('deleted', event.src_path)

    def on_moved(self, event):
        if self._contains_git(event.src_path) or self._contains_git(event.dest_path):
            return
        if not event.is_directory:
            self._record('deleted', event.src_path)
            self._record('changed', event.dest_path)


# ---------------------------------------------------------------------------
# WatchEngine — one per watched directory
# ---------------------------------------------------------------------------

class WatchEngine:
    """Manages watching a single directory and committing changes to a shadow git repo."""

    def __init__(self, watch_path: str,
                 log: Callable[[str], None] | None = None,
                 shadows_base: Path = None,  # required — set by caller
                 initial_patterns: list[str] | None = None) -> None:
        self.watch_path = Path(watch_path).resolve()
        self.shadow_path = (shadows_base / shadow_name(watch_path)).resolve()
        self._log = log or print
        self._observer: Observer | None = None
        self._handler: _Handler | None = None
        self._running = False
        self._paused = False

        self.shadow_path.mkdir(parents=True, exist_ok=True)
        ignore_file = self.shadow_path / 'ignore'
        if not ignore_file.exists():
            # First time: caller provides patterns, or fall back to General preset
            patterns = initial_patterns if initial_patterns is not None else IGNORE_PRESETS['General']
            self.ignore = IgnoreFilter(ignore_file)
            self.ignore.set_patterns(patterns)
        else:
            self.ignore = IgnoreFilter(ignore_file)

    @property
    def is_running(self) -> bool:
        return self._running

    def start(self) -> None:
        if self._running:
            return
        self._running = True
        self._init_thread = threading.Thread(target=self._init_and_watch, daemon=True)
        self._init_thread.start()

    def _init_and_watch(self) -> None:
        try:
            _init_shadow_repo(self.watch_path, self.shadow_path, self.ignore, self._log)
            _sync_git_exclude(self.shadow_path, self.ignore)

            self._handler = _Handler(self.watch_path, self.shadow_path, self.ignore, self._log)
            self._observer = Observer()
            self._observer.schedule(self._handler, str(self.watch_path), recursive=True)
            self._observer.start()
            self._log(f'Watching: {self.watch_path}')
        except Exception as e:
            self._log(f'ERROR: {e}')
            self._running = False

    @property
    def is_paused(self) -> bool:
        return self._paused

    def pause(self) -> None:
        if not self._running or self._paused:
            return
        if self._observer:
            self._observer.stop()
            self._observer.join(timeout=5)
            self._observer = None
        self._handler = None
        self._paused = True
        self._log(f'Paused: {self.watch_path}')

    def resume(self) -> None:
        if not self._running or not self._paused:
            return
        self._paused = False
        self._handler = _Handler(self.watch_path, self.shadow_path, self.ignore, self._log)
        self._observer = Observer()
        self._observer.schedule(self._handler, str(self.watch_path), recursive=True)
        self._observer.start()
        self._log(f'Resumed: {self.watch_path}')

    def sync_exclude(self) -> None:
        _sync_git_exclude(self.shadow_path, self.ignore)

    def stop(self) -> None:
        if not self._running:
            return
        if self._observer:
            self._observer.stop()
            self._observer.join(timeout=5)
            self._observer = None
        self._handler = None
        self._running = False
        self._paused = False
        self._log(f'Stopped: {self.watch_path}')


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    path = 'd:/dev/baconsaver'
    shadows_base = Path.home() / 'BaconSaverData' / 'shadows'

    engine = WatchEngine(path, shadows_base=shadows_base)
    engine.start()
    print(f'Shadow: {engine.shadow_path}')
    print('Press Ctrl+C to stop.')

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        engine.stop()