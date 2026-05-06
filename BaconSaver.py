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
DEFAULT_SHADOWS_BASE = Path('E:/BaconSaverData/shadows')

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

def shadow_name(watch_path: str) -> str:
    return re.sub(r'[^a-zA-Z0-9]', '_', watch_path).strip('_')


def _git(args: list[str], git_dir: Path, work_tree: Path | None = None,
         check: bool = True) -> subprocess.CompletedProcess:
    cmd = ['git', f'--git-dir={git_dir}']
    if work_tree is not None:
        cmd.append(f'--work-tree={work_tree}')
    cmd += args
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f'git {args[0]} failed:\n{r.stderr}')
    return r


def _sync_git_exclude(shadow: Path, ignore: IgnoreFilter) -> None:
    exclude = shadow / '.git' / 'info' / 'exclude'
    exclude.parent.mkdir(exist_ok=True)
    exclude.write_text(ignore.as_git_exclude())


def _init_shadow_repo(watch: Path, shadow: Path, ignore: IgnoreFilter,
                      log: Callable[[str], None]) -> None:
    git_dir = shadow / '.git'
    if git_dir.exists():
        _sync_git_exclude(shadow, ignore)
        return

    # Init inside the shadow directory — never touches the watched directory at all.
    # The watched dir may already have its own .git which we must not disturb.
    shadow.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(['git', 'init', str(shadow)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f'git init failed:\n{r.stderr}')

    _git(['config', 'user.name', 'BaconSaver'], git_dir)
    _git(['config', 'user.email', 'baconsaver@local'], git_dir)
    _git(['config', 'core.worktree', str(watch)], git_dir)
    _sync_git_exclude(shadow, ignore)

    log(f'Initialized shadow repo: {git_dir}')
    log('Taking initial snapshot...')
    _git(['add', '-A'], git_dir, watch)
    r = _git(['status', '--porcelain'], git_dir, watch, check=False)
    if r.stdout.strip():
        _git(['commit', '-m', 'BaconSaver: initial snapshot'], git_dir, watch)
        log('Initial snapshot committed.')
    else:
        log('Nothing to snapshot.')


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

    def _schedule(self) -> None:
        if self._timer:
            self._timer.cancel()
        self._timer = threading.Timer(self.DEBOUNCE, self._commit)
        self._timer.daemon = True
        self._timer.start()

    def _commit(self) -> None:
        with self._lock:
            pending = dict(self._pending)
            self._pending.clear()

        if not pending:
            return

        try:
            _git(['add', '-A'], self.git_dir, self.watch)
            r = _git(['status', '--porcelain'], self.git_dir, self.watch, check=False)
            if r.stdout.strip():
                ts = time.strftime('%Y-%m-%d %H:%M:%S')
                for line in r.stdout.strip().splitlines():
                    self._log(f'  {line.strip()}')
                _git(['commit', '-m', f'BaconSaver {ts}'], self.git_dir, self.watch)
                n = len(r.stdout.strip().splitlines())
                self._log(f'[{ts}] Committed {n} file(s).')
        except RuntimeError as e:
            self._log(str(e))

    def _record(self, action: str, abs_path: str) -> None:
        p = Path(abs_path).resolve()
        if p.is_relative_to(self.shadow):
            return
        try:
            rel = str(p.relative_to(self.watch))
        except ValueError:
            return
        if self._ignore.is_ignored(rel):
            return
        with self._lock:
            self._pending[rel] = action
        self._schedule()

    def on_created(self, event):
        if not event.is_directory:
            self._record('changed', event.src_path)

    def on_modified(self, event):
        if not event.is_directory:
            self._record('changed', event.src_path)

    def on_deleted(self, event):
        if not event.is_directory:
            self._record('deleted', event.src_path)

    def on_moved(self, event):
        if not event.is_directory:
            self._record('deleted', event.src_path)
            self._record('upsert', event.dest_path)


# ---------------------------------------------------------------------------
# WatchEngine — one per watched directory
# ---------------------------------------------------------------------------

class WatchEngine:
    """Manages watching a single directory and committing changes to a shadow git repo."""

    def __init__(self, watch_path: str,
                 log: Callable[[str], None] | None = None,
                 shadows_base: Path = DEFAULT_SHADOWS_BASE,
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

    engine = WatchEngine(path)
    engine.start()
    print(f'Shadow: {engine.shadow_path}')
    print('Press Ctrl+C to stop.')

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        engine.stop()
