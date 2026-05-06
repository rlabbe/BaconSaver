import fnmatch
import json
import os
import sys
from pathlib import Path

from PyQt5.QtCore import Qt, pyqtSignal, QObject
from PyQt5.QtGui import QFont
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QHBoxLayout, QVBoxLayout,
    QSplitter, QPlainTextEdit, QListWidget, QListWidgetItem, QPushButton,
    QFileDialog, QStatusBar, QMessageBox, QDialog, QInputDialog,
    QLabel, QDialogButtonBox, QCheckBox,
)

from BaconSaver import WatchEngine, IgnoreFilter, IGNORE_PRESETS, ALWAYS_IGNORED, APP_DIR, DEFAULT_SHADOWS_BASE


CONFIG_FILE = APP_DIR / 'config.json'


# ---------------------------------------------------------------------------
# Thread-safe log bridge: WatchEngine callback -> Qt signal
# ---------------------------------------------------------------------------

class _LogBridge(QObject):
    message = pyqtSignal(str)


# ---------------------------------------------------------------------------
# Shared pattern list widget with add/remove
# ---------------------------------------------------------------------------

class _PatternListWidget(QWidget):
    """Reusable widget: QListWidget of patterns with Add/Remove buttons."""

    def __init__(self, patterns: list[str], parent=None):
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        self._list = QListWidget()
        for p in patterns:
            self._list.addItem(p)
        layout.addWidget(self._list)

        btn_row = QHBoxLayout()
        add_btn = QPushButton('Add...')
        add_btn.clicked.connect(self._add)
        remove_btn = QPushButton('Remove')
        remove_btn.clicked.connect(self._remove)
        btn_row.addWidget(add_btn)
        btn_row.addWidget(remove_btn)
        btn_row.addStretch()
        layout.addLayout(btn_row)

    def _add(self):
        text, ok = QInputDialog.getText(self.window(), 'Add Pattern', 'Pattern:')
        if ok and text.strip():
            self._list.addItem(text.strip())

    def _remove(self):
        row = self._list.currentRow()
        if row >= 0:
            self._list.takeItem(row)

    def set_patterns(self, patterns: list[str]):
        self._list.clear()
        for p in patterns:
            self._list.addItem(p)

    def get_patterns(self) -> list[str]:
        return [self._list.item(i).text() for i in range(self._list.count())]


# ---------------------------------------------------------------------------
# Add directory dialog — shown before git init
# ---------------------------------------------------------------------------

class AddDirectoryDialog(QDialog):
    """Pick a directory, choose a preset, edit patterns, then confirm."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle('Add Watched Directory')
        self.setMinimumSize(500, 450)

        layout = QVBoxLayout(self)

        # Directory picker row
        dir_row = QHBoxLayout()
        dir_row.addWidget(QLabel('Directory:'))
        self._dir_label = QLabel('<none>')
        self._dir_label.setStyleSheet('font-weight: bold;')
        dir_row.addWidget(self._dir_label, 1)
        browse_btn = QPushButton('Browse...')
        browse_btn.clicked.connect(self._browse)
        dir_row.addWidget(browse_btn)
        layout.addLayout(dir_row)

        # Preset checkboxes — additive, check multiple to merge
        preset_row = QHBoxLayout()
        preset_row.addWidget(QLabel('Presets:'))
        self._preset_checks: dict[str, QCheckBox] = {}
        for name in IGNORE_PRESETS:
            cb = QCheckBox(name)
            cb.toggled.connect(self._presets_changed)
            preset_row.addWidget(cb)
            self._preset_checks[name] = cb
        preset_row.addStretch()
        layout.addLayout(preset_row)

        # Pattern help
        layout.addWidget(QLabel(
            'Patterns without / match any path component.\n'
            'Patterns with / match the full relative path.\n'
            'Wildcards: *  ?  [seq]'
        ))

        # Editable pattern list
        self._pattern_widget = _PatternListWidget(IGNORE_PRESETS['General'])
        layout.addWidget(self._pattern_widget, 1)

        # Preview + OK / Cancel
        bottom_row = QHBoxLayout()
        self._preview_btn = QPushButton('Preview Files...')
        self._preview_btn.setEnabled(False)
        self._preview_btn.clicked.connect(self._preview_files)
        bottom_row.addWidget(self._preview_btn)
        bottom_row.addStretch()
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        self._ok_btn = buttons.button(QDialogButtonBox.Ok)
        self._ok_btn.setEnabled(False)
        bottom_row.addWidget(buttons)
        layout.addLayout(bottom_row)

        self._selected_path: str | None = None

        # General checked by default — must be after _pattern_widget exists
        self._preset_checks['General'].setChecked(True)

    def _browse(self):
        path = QFileDialog.getExistingDirectory(self, 'Select Directory to Watch')
        if path:
            self._selected_path = str(Path(path).resolve())
            self._dir_label.setText(self._selected_path)
            self._ok_btn.setEnabled(True)
            self._preview_btn.setEnabled(True)

    def _presets_changed(self):
        # Merge all checked presets, preserving order, deduplicating
        merged = []
        seen = set()
        for name, cb in self._preset_checks.items():
            if cb.isChecked():
                for p in IGNORE_PRESETS[name]:
                    if p not in seen:
                        merged.append(p)
                        seen.add(p)
        self._pattern_widget.set_patterns(merged)

    def _preview_files(self):
        if not self._selected_path:
            return
        patterns = self._pattern_widget.get_patterns()
        watch = Path(self._selected_path)

        # Split patterns into component vs path, same logic as IgnoreFilter
        comp_pats = list(ALWAYS_IGNORED)
        path_pats = []
        for p in patterns:
            if '/' in p.rstrip('/'):
                path_pats.append(p.rstrip('/'))
            else:
                comp_pats.append(p)

        watched = []
        ignored_count = 0
        for root, dirs, files in os.walk(watch):
            # Prune ignored directories in-place so os.walk skips them
            rel_root = os.path.relpath(root, watch)
            parts = Path(rel_root).parts if rel_root != '.' else ()
            pruned = []
            for d in dirs:
                skip = False
                for pat in comp_pats:
                    if fnmatch.fnmatch(d, pat):
                        skip = True
                        break
                if not skip:
                    pruned.append(d)
            dirs[:] = pruned

            for f in files:
                rel = os.path.relpath(os.path.join(root, f), watch)
                file_parts = Path(rel).parts
                skip = False
                for pat in comp_pats:
                    for part in file_parts:
                        if fnmatch.fnmatch(part, pat):
                            skip = True
                            break
                    if skip:
                        break
                if not skip:
                    norm = rel.replace('\\', '/')
                    for pat in path_pats:
                        if fnmatch.fnmatch(norm, pat):
                            skip = True
                            break
                if skip:
                    ignored_count += 1
                else:
                    watched.append(rel)

        dlg = QDialog(self)
        dlg.setWindowTitle(f'Preview: {len(watched)} files watched, {ignored_count} ignored')
        dlg.setMinimumSize(600, 400)
        layout = QVBoxLayout(dlg)
        text = QPlainTextEdit()
        text.setReadOnly(True)
        text.setFont(QFont('Consolas', 9))
        text.setPlainText('\n'.join(watched))
        layout.addWidget(text)
        close_btn = QPushButton('Close')
        close_btn.clicked.connect(dlg.accept)
        layout.addWidget(close_btn)
        dlg.exec_()

    def get_path(self) -> str | None:
        return self._selected_path

    def get_patterns(self) -> list[str]:
        return self._pattern_widget.get_patterns()


# ---------------------------------------------------------------------------
# Ignore dialog — for editing an already-watched directory
# ---------------------------------------------------------------------------

class IgnoreDialog(QDialog):
    def __init__(self, ignore: IgnoreFilter, parent=None):
        super().__init__(parent)
        self._ignore = ignore
        self.setWindowTitle('Edit Ignore Patterns')
        self.setMinimumSize(450, 400)

        layout = QVBoxLayout(self)

        # Load preset buttons — adds patterns from preset without removing existing
        preset_row = QHBoxLayout()
        preset_row.addWidget(QLabel('Add preset:'))
        for name in IGNORE_PRESETS:
            btn = QPushButton(name)
            btn.clicked.connect(lambda checked, n=name: self._add_preset(n))
            preset_row.addWidget(btn)
        preset_row.addStretch()
        layout.addLayout(preset_row)

        layout.addWidget(QLabel(
            'Patterns without / match any path component.\n'
            'Patterns with / match the full relative path.\n'
            'Wildcards: *  ?  [seq]'
        ))

        self._pattern_widget = _PatternListWidget(ignore.patterns)
        layout.addWidget(self._pattern_widget, 1)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def _add_preset(self, name: str):
        existing = set(self._pattern_widget.get_patterns())
        merged = self._pattern_widget.get_patterns()
        for p in IGNORE_PRESETS[name]:
            if p not in existing:
                merged.append(p)
                existing.add(p)
        self._pattern_widget.set_patterns(merged)

    def get_patterns(self) -> list[str]:
        return self._pattern_widget.get_patterns()


# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('BaconSaver')
        self.setMinimumSize(800, 500)
        self.resize(1000, 600)

        self._engines: dict[str, WatchEngine] = {}  # watch_path_str -> engine
        self._bridges: dict[str, _LogBridge] = {}
        self._shadows_base: Path = DEFAULT_SHADOWS_BASE

        # --- Central widget with splitter ---
        splitter = QSplitter(Qt.Horizontal)

        # Left panel: directory list + buttons
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(4, 4, 4, 4)

        left_layout.addWidget(QLabel('Watched Directories'))
        self._dir_list = QListWidget()
        self._dir_list.setSelectionMode(QListWidget.SingleSelection)
        left_layout.addWidget(self._dir_list)

        add_btn = QPushButton('Add Directory...')
        add_btn.clicked.connect(self._add_directory)
        left_layout.addWidget(add_btn)

        remove_btn = QPushButton('Remove')
        remove_btn.clicked.connect(self._remove_directory)
        left_layout.addWidget(remove_btn)

        pause_btn = QPushButton('Pause / Resume')
        pause_btn.clicked.connect(self._toggle_pause)
        left_layout.addWidget(pause_btn)

        ignore_btn = QPushButton('Edit Ignores...')
        ignore_btn.clicked.connect(self._edit_ignores)
        left_layout.addWidget(ignore_btn)

        restore_btn = QPushButton('Restore...')
        restore_btn.setEnabled(False)
        restore_btn.setToolTip('Coming soon')
        left_layout.addWidget(restore_btn)
        self._restore_btn = restore_btn

        splitter.addWidget(left)

        # Right panel: console
        self._console = QPlainTextEdit()
        self._console.setReadOnly(True)
        self._console.setFont(QFont('Consolas', 10))
        self._console.setStyleSheet(
            'QPlainTextEdit { background-color: #1e1e1e; color: #cccccc; }'
        )
        self._console.setMaximumBlockCount(10000)
        splitter.addWidget(self._console)

        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([220, 780])

        self.setCentralWidget(splitter)

        # Status bar
        self._status = QStatusBar()
        self.setStatusBar(self._status)
        self._update_status()

        # Restore saved directories
        self._load_config()

    # --- Logging ---

    def _make_log_callback(self, label: str) -> tuple[_LogBridge, callable]:
        bridge = _LogBridge()
        bridge.message.connect(lambda msg, lbl=label: self._append_log(lbl, msg))
        return bridge, lambda msg, b=bridge: b.message.emit(msg)

    def _append_log(self, label: str, msg: str):
        self._console.appendPlainText(f'[{label}] {msg}')

    # --- Directory management ---

    def _add_directory(self):
        dlg = AddDirectoryDialog(self)
        if dlg.exec_() != QDialog.Accepted:
            return
        resolved = dlg.get_path()
        if not resolved:
            return
        if resolved in self._engines:
            QMessageBox.information(self, 'Already Watching',
                                   f'{resolved} is already being watched.')
            return
        self._start_engine(resolved, initial_patterns=dlg.get_patterns())
        self._save_config()

    def _remove_directory(self):
        row = self._dir_list.currentRow()
        if row < 0:
            return
        item = self._dir_list.item(row)
        path = item.data(Qt.UserRole)
        reply = QMessageBox.question(
            self, 'Remove Directory',
            f'Stop watching {path}?\n\n'
            'History is preserved in the shadow repo and will be reused\n'
            'if you add this directory again.',
            QMessageBox.Yes | QMessageBox.No
        )
        if reply != QMessageBox.Yes:
            return
        self._stop_engine(path)
        self._dir_list.takeItem(row)
        self._save_config()
        self._update_status()

    def _edit_ignores(self):
        row = self._dir_list.currentRow()
        if row < 0:
            QMessageBox.information(self, 'No Selection',
                                   'Select a directory first.')
            return
        path = self._dir_list.item(row).data(Qt.UserRole)
        engine = self._engines.get(path)
        if not engine:
            return
        dlg = IgnoreDialog(engine.ignore, self)
        if dlg.exec_() == QDialog.Accepted:
            engine.ignore.set_patterns(dlg.get_patterns())
            self._append_log(Path(path).name, 'Ignore patterns updated.')

    def _toggle_pause(self):
        row = self._dir_list.currentRow()
        if row < 0:
            return
        item = self._dir_list.item(row)
        path = item.data(Qt.UserRole)
        engine = self._engines.get(path)
        if not engine:
            return
        if engine.is_paused:
            engine.resume()
            item.setText(path)
        else:
            engine.pause()
            item.setText(f'{path}  (paused)')
        self._save_config()

    # --- Engine lifecycle ---

    def _start_engine(self, path: str, initial_patterns: list[str] | None = None):
        label = Path(path).name
        bridge, log_fn = self._make_log_callback(label)
        self._bridges[path] = bridge

        engine = WatchEngine(path, log=log_fn, shadows_base=self._shadows_base, initial_patterns=initial_patterns)
        self._engines[path] = engine
        try:
            engine.start()
        except Exception as e:
            self._append_log(label, f'ERROR: {e}')

        item = QListWidgetItem(path)
        item.setData(Qt.UserRole, path)
        self._dir_list.addItem(item)
        self._update_status()

    def _stop_engine(self, path: str):
        engine = self._engines.pop(path, None)
        if engine:
            engine.stop()
        self._bridges.pop(path, None)

    # --- Config persistence ---

    def _save_config(self):
        self._shadows_base.mkdir(parents=True, exist_ok=True)
        entries = []
        for path, engine in self._engines.items():
            entries.append({'path': path, 'paused': engine.is_paused})
        data = {
            'shadows_base': str(self._shadows_base),
            'watched': entries,
        }
        CONFIG_FILE.write_text(json.dumps(data, indent=2))

    def _load_config(self):
        if not CONFIG_FILE.exists():
            return
        try:
            data = json.loads(CONFIG_FILE.read_text())
            sb = data.get('shadows_base')
            if sb:
                self._shadows_base = Path(sb)
            for entry in data.get('watched', []):
                # Support old format (plain string list) and new format (dict list)
                if isinstance(entry, str):
                    path, paused = entry, False
                else:
                    path, paused = entry['path'], entry.get('paused', False)
                if Path(path).is_dir():
                    self._start_engine(path)
                    if paused:
                        engine = self._engines.get(path)
                        if engine:
                            engine.pause()
                            # Update list item text
                            for i in range(self._dir_list.count()):
                                item = self._dir_list.item(i)
                                if item.data(Qt.UserRole) == path:
                                    item.setText(f'{path}  (paused)')
                                    break
        except (json.JSONDecodeError, KeyError):
            pass

    # --- Status ---

    def _update_status(self):
        n = len(self._engines)
        if n == 0:
            self._status.showMessage('Not watching any directories')
        elif n == 1:
            self._status.showMessage('Watching 1 directory')
        else:
            self._status.showMessage(f'Watching {n} directories')

    # --- Cleanup ---

    def closeEvent(self, event):
        self._save_config()
        for path in list(self._engines):
            self._stop_engine(path)
        event.accept()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    app = QApplication(sys.argv)
    app.setStyle('Fusion')
    window = MainWindow()
    window.show()
    sys.exit(app.exec_())
