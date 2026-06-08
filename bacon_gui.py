import fnmatch
import json
import os
import subprocess
import sys
import time
from pathlib import Path



from PyQt6.QtCore import Qt, pyqtSignal, QObject, QEvent
from PyQt6.QtGui import QFont, QAction, QShortcut, QKeySequence, QTextCursor, QIcon
from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QHBoxLayout, QVBoxLayout,
    QSplitter, QPlainTextEdit, QListWidget, QListWidgetItem, QPushButton,
    QFileDialog, QStatusBar, QMessageBox, QDialog, QInputDialog,
    QLabel, QDialogButtonBox, QCheckBox, QTreeWidget, QTreeWidgetItem,
    QRadioButton, QButtonGroup, QHeaderView, QTextEdit, QFontDialog, QMenu,
    QSystemTrayIcon,
)

from BaconSaver import (
    WatchEngine, IgnoreFilter, IGNORE_PRESETS, ALWAYS_IGNORED, APP_DIR,
    get_commit_log, get_commit_files, get_file_at_commit, get_full_tree_at_commit,
    get_diff_for_commit, export_files, shadow_name, git_version,
)


CONFIG_FILE = APP_DIR / 'config.json'
LOG_DIR = APP_DIR / 'logs'


def _file_log(msg: str) -> None:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    path = LOG_DIR / 'baconsaver.log'
    line = f'{time.strftime("%Y-%m-%d %H:%M:%S")}  {msg}\n'
    with open(path, 'a') as f:
        f.write(line)


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
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        self._ok_btn = buttons.button(QDialogButtonBox.StandardButton.Ok)
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
        dlg.exec()

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

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
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
# Restore dialog
# ---------------------------------------------------------------------------

class RestoreDialog(QDialog):
    """Browse commit history and export files from any point in time."""

    def __init__(self, watch_path: str, shadow_path: Path, parent=None):
        super().__init__(parent)
        self._watch_path = watch_path
        self._shadow_path = shadow_path
        self._git_dir = shadow_path / '.git'
        self._work_tree = Path(watch_path)
        self._current_hash: str | None = None
        self._selected_file: str | None = None
        self._selected_status: str = ''
        self._tabs_as_spaces = True
        self._tab_width = 4

        self.setWindowTitle(f'Restore — {watch_path}')
        self.setMinimumSize(600, 400)
        size = self._load_size()
        self.resize(size[0], size[1])

        outer = QVBoxLayout(self)

        splitter = QSplitter(Qt.Orientation.Horizontal)

        # --- Left panel: commit timeline ---
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(4, 4, 4, 4)
        left_layout.addWidget(QLabel('Snapshots'))
        self._commit_list = QListWidget()
        self._commit_list.currentRowChanged.connect(self._on_commit_selected)
        left_layout.addWidget(self._commit_list)
        splitter.addWidget(left)

        # --- Middle panel: file list ---
        mid = QWidget()
        mid_layout = QVBoxLayout(mid)
        mid.setMinimumWidth(0)
        mid_layout.setContentsMargins(4, 4, 4, 4)

        mode_row = QHBoxLayout()
        self._mode_changed = QRadioButton('Changed files')
        self._mode_all = QRadioButton('Full snapshot')
        self._mode_changed.setChecked(True)
        mode_group = QButtonGroup(self)
        mode_group.addButton(self._mode_changed)
        mode_group.addButton(self._mode_all)
        self._mode_changed.toggled.connect(self._refresh_file_list)
        mode_row.addWidget(self._mode_changed)
        mode_row.addWidget(self._mode_all)
        mode_row.addStretch()

        self._select_all_btn = QPushButton('Select All')
        self._select_all_btn.clicked.connect(self._select_all)
        self._select_none_btn = QPushButton('Select None')
        self._select_none_btn.clicked.connect(self._select_none)
        mode_row.addWidget(self._select_all_btn)
        mode_row.addWidget(self._select_none_btn)
        mid_layout.addLayout(mode_row)

        self._file_tree = QTreeWidget()
        self._file_tree.setHeaderLabels(['File', 'Status'])
        self._file_tree.header().setStretchLastSection(False)
        self._file_tree.header().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self._file_tree.header().setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        self._file_tree.itemClicked.connect(self._on_file_clicked)
        self._file_tree.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self._file_tree.customContextMenuRequested.connect(self._file_context_menu)
        mid_layout.addWidget(self._file_tree)
        splitter.addWidget(mid)

        # --- Right panel: preview / diff ---
        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.setContentsMargins(4, 4, 4, 4)
        view_row = QHBoxLayout()
        self._view_content = QRadioButton('Content')
        self._view_diff = QRadioButton('Diff')
        self._view_content.setChecked(True)
        view_group = QButtonGroup(self)
        view_group.addButton(self._view_content)
        view_group.addButton(self._view_diff)
        self._view_content.toggled.connect(self._refresh_preview)
        view_row.addWidget(self._view_content)
        view_row.addWidget(self._view_diff)
        view_row.addStretch()
        font_btn = QPushButton('Font...')
        font_btn.clicked.connect(self._pick_font)
        view_row.addWidget(font_btn)
        right_layout.addLayout(view_row)
        self._preview_font = self._load_preview_font()
        self._preview = QTextEdit()
        self._preview.setReadOnly(True)
        self._preview.setLineWrapMode(QTextEdit.LineWrapMode.NoWrap)
        self._preview.setFont(self._preview_font)
        self._preview.setStyleSheet(
            'QTextEdit { background-color: #1e1e1e; color: #cccccc; }'
        )
        right_layout.addWidget(self._preview)
        goto_shortcut = QShortcut(QKeySequence('Ctrl+G'), self)
        goto_shortcut.activated.connect(self._goto_line)
        splitter.addWidget(right)

        self._splitter = splitter
        outer.addWidget(splitter, 1)

        # --- Bottom: export ---
        bottom = QHBoxLayout()
        self._file_count_label = QLabel('')
        bottom.addWidget(self._file_count_label)
        bottom.addStretch()
        self._export_btn = QPushButton('Export Selected')
        self._export_btn.setEnabled(False)
        self._export_btn.clicked.connect(self._export)
        bottom.addWidget(self._export_btn)
        close_btn = QPushButton('Close')
        close_btn.clicked.connect(self.reject)
        bottom.addWidget(close_btn)
        outer.addLayout(bottom)

        self._commits: list[dict] = []
        self._load_commits()

    def showEvent(self, event):
        super().showEvent(event)
        if not hasattr(self, '_splitter_fitted'):
            self._splitter_fitted = True
            from PyQt6.QtCore import QTimer
            QTimer.singleShot(0, self._fit_splitter)

    def _fit_splitter(self):
            # Temporarily switch to ResizeToContents to measure the ACTUAL text width
            self._file_tree.header().setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)

            # 1. Calculate width for the Commit List (Left)
            max_w = 0
            model = self._commit_list.model()
            for i in range(model.rowCount()):
                w = self._commit_list.sizeHintForIndex(model.index(i, 0)).width()
                if w > max_w:
                    max_w = w

            left_w = (max_w +
                      self._commit_list.verticalScrollBar().sizeHint().width() +
                      (self._commit_list.frameWidth() * 2) + 15)

            # 2. Calculate width for the File Tree (Middle)
            self._file_tree.resizeColumnToContents(0)
            self._file_tree.resizeColumnToContents(1)
            mid_w = (self._file_tree.columnWidth(0) +
                     self._file_tree.columnWidth(1) +
                     (self._file_tree.frameWidth() * 2) + 25)

            # 3. Apply sizes and Stretch Factors
            # Using a very large number (10000) for the right pane forces the
            # first two to collapse to their exact calculated widths.
            self._splitter.setSizes([left_w, mid_w, 10000])

            # Ensure only the right pane (index 2) expands when you resize the window
            self._splitter.setStretchFactor(0, 0)
            self._splitter.setStretchFactor(1, 0)
            self._splitter.setStretchFactor(2, 1)

            # Optional: Switch back to Stretch if you want the filename column
            # to fill whatever width the middle pane has.
            self._file_tree.header().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)

    @staticmethod
    def _format_timestamp(raw: str) -> str:
        """'2026-05-05 14:03:53 -0700' -> '2026-05-05  14:03:53'"""
        parts = raw.split()
        if len(parts) >= 2:
            return f'{parts[0]}  {parts[1]}'
        return raw

    def _load_commits(self):
        try:
            self._commits = get_commit_log(self._git_dir, self._work_tree)
        except Exception as e:
            self._preview.setPlainText(f'Error loading commits: {e}')
            return
        self._commit_list.clear()
        for c in self._commits:
            ts = self._format_timestamp(c['timestamp'])
            try:
                changed = get_commit_files(self._git_dir, self._work_tree, c['hash'])
            except Exception:
                changed = []
            if len(changed) == 1:
                detail = Path(changed[0]['path']).name
            else:
                detail = f'{len(changed)} files'
            self._commit_list.addItem(f'{ts}   ({detail})')
        if self._commits:
            self._commit_list.setCurrentRow(0)

    def _on_commit_selected(self, row: int):
        if row < 0 or row >= len(self._commits):
            self._current_hash = None
            self._file_tree.clear()
            self._preview.clear()
            self._export_btn.setEnabled(False)
            return
        self._current_hash = self._commits[row]['hash']
        self._refresh_file_list()

    def _refresh_file_list(self):
        self._file_tree.clear()
        self._preview.clear()
        if not self._current_hash:
            return

        try:
            if self._mode_changed.isChecked():
                files = get_commit_files(self._git_dir, self._work_tree, self._current_hash)
            else:
                files = [{'path': fp, 'status': ''} for fp in get_full_tree_at_commit(self._git_dir, self._current_hash)]
        except Exception as e:
            self._preview.setPlainText(f'Error loading files: {e}')
            return

        if self._mode_changed.isChecked():
            for f in files:
                item = QTreeWidgetItem([f['path'], f['status']])
                item.setCheckState(0, Qt.CheckState.Checked)
                item.setData(0, Qt.ItemDataRole.UserRole, f['path'])
                self._file_tree.addTopLevelItem(item)
        else:
            files = get_full_tree_at_commit(self._git_dir, self._current_hash)
            for fp in files:
                item = QTreeWidgetItem([fp, ''])
                item.setCheckState(0, Qt.CheckState.Checked)
                item.setData(0, Qt.ItemDataRole.UserRole, fp)
                self._file_tree.addTopLevelItem(item)

        n = self._file_tree.topLevelItemCount()
        self._file_count_label.setText(f'{n} file(s)')
        self._export_btn.setEnabled(n > 0)

        if n == 1:
            item = self._file_tree.topLevelItem(0)
            self._selected_file = item.data(0, Qt.ItemDataRole.UserRole)
            self._selected_status = item.text(1)
            self._file_tree.setCurrentItem(item)
            self._refresh_preview()

    def _select_all(self):
        for i in range(self._file_tree.topLevelItemCount()):
            self._file_tree.topLevelItem(i).setCheckState(0, Qt.CheckState.Checked)

    def _select_none(self):
        for i in range(self._file_tree.topLevelItemCount()):
            self._file_tree.topLevelItem(i).setCheckState(0, Qt.CheckState.Unchecked)

    @staticmethod
    def _is_binary(data: bytes) -> bool:
        if not data:
            return False
        if data[:2] in (b'\xff\xfe', b'\xfe\xff') or data[:3] == b'\xef\xbb\xbf':
            return False
        chunk = data[:8192]
        # Same heuristic as git: binary if null bytes present in the sample
        return b'\x00' in chunk

    @staticmethod
    def _visible_whitespace(text: str) -> str:
        return text.replace('\t', '→\t').replace(' ', '·')

    @staticmethod
    def _html_escape(text: str) -> str:
        return text.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')

    def _show_diff_html(self, diff_text: str):
        lines = []
        for raw_line in diff_text.splitlines():
            escaped = self._html_escape(raw_line)
            visible = self._visible_whitespace(escaped)
            if raw_line.startswith('+++') or raw_line.startswith('---'):
                lines.append(f'<span style="color:#569cd6;font-weight:bold">{visible}</span>')
            elif raw_line.startswith('@@'):
                lines.append(f'<span style="color:#c586c0">{visible}</span>')
            elif raw_line.startswith('+'):
                lines.append(f'<span style="color:#4ec9b0;background-color:#1e3a1e">{visible}</span>')
            elif raw_line.startswith('-'):
                lines.append(f'<span style="color:#f44747;background-color:#3a1e1e">{visible}</span>')
            else:
                lines.append(f'<span style="color:#cccccc">{visible}</span>')
        family = self._preview_font.family()
        size = self._preview_font.pointSize()
        html = (
            f'<pre style="font-family:{family};font-size:{size}pt;background-color:#1e1e1e;'
            'color:#cccccc;margin:0;padding:4px">'
            + '<br>'.join(lines)
            + '</pre>'
        )
        self._preview.setHtml(html)

    def _file_context_menu(self, pos):
        try:
            item = self._file_tree.itemAt(pos)
            if not item:
                return
            rel_path = item.data(0, Qt.ItemDataRole.UserRole)
            if not rel_path:
                return
            full_path = Path(self._watch_path) / rel_path

            menu = QMenu(self)
            copy_action = menu.addAction('Copy Full Path')
            explorer_action = menu.addAction('Open in Explorer')
            code_action = menu.addAction('Open with Code')

            action = menu.exec_(self._file_tree.viewport().mapToGlobal(pos))
            if action == copy_action:
                QApplication.clipboard().setText(str(full_path))
            elif action == explorer_action:
                if full_path.exists():
                    subprocess.Popen(['explorer', '/select,', str(full_path)])
                else:
                    subprocess.Popen(['explorer', str(full_path.parent)])
            elif action == code_action:
                subprocess.Popen(f'code "{full_path}"', shell=True)
        except Exception:
            pass

    def _goto_line(self):
        try:
            total = self._preview.document().blockCount()
            if total <= 0:
                return
            line, ok = QInputDialog.getInt(self, 'Go to Line', f'Line (1–{total}):', 1, 1, total)
            if ok:
                block = self._preview.document().findBlockByLineNumber(line - 1)
                if block.isValid():
                    cursor = self._preview.textCursor()
                    cursor.setPosition(block.position())
                    cursor.movePosition(QTextCursor.MoveOperation.EndOfBlock, QTextCursor.MoveMode.KeepAnchor)
                    self._preview.setTextCursor(cursor)
                    self._preview.setFocus()
                    # Scroll so the line is at the top
                    self._preview.verticalScrollBar().setValue(
                        self._preview.verticalScrollBar().value()
                        + self._preview.cursorRect().top()
                    )
        except Exception:
            pass

    def _on_file_clicked(self, item: QTreeWidgetItem, column: int):
        self._selected_file = item.data(0, Qt.ItemDataRole.UserRole)
        self._selected_status = item.text(1)
        self._refresh_preview()

    def _refresh_preview(self):
        fp = getattr(self, '_selected_file', None)
        if not fp or not self._current_hash:
            self._preview.clear()
            return

        try:
            status = getattr(self, '_selected_status', '')

            if status == 'D':
                self._preview.setPlainText('[File deleted in this snapshot]')
                return

            if self._view_diff.isChecked():
                diff_text = get_diff_for_commit(self._git_dir, self._work_tree,
                                                self._current_hash, fp)
                if diff_text.strip():
                    self._show_diff_html(diff_text)
                else:
                    self._preview.setPlainText('[No diff — file unchanged or binary]')
                return

            content = get_file_at_commit(self._git_dir, self._current_hash, fp)
            if self._is_binary(content):
                self._preview.setPlainText(f'[Binary file — {len(content):,} bytes]')
            else:
                if content[:2] == b'\xff\xfe':
                    text = content.decode('utf-16-le', errors='replace')
                elif content[:2] == b'\xfe\xff':
                    text = content.decode('utf-16-be', errors='replace')
                else:
                    text = content.decode('utf-8', errors='replace')
                if self._tabs_as_spaces:
                    text = text.replace('\t', ' ' * self._tab_width)
                self._preview.setPlainText(text)
        except Exception as e:
            self._preview.setPlainText(f'Error: {e}')

    def _export(self):
        try:
            if not self._current_hash:
                return
            checked = []
            for i in range(self._file_tree.topLevelItemCount()):
                item = self._file_tree.topLevelItem(i)
                if item.checkState(0) == Qt.CheckState.Checked:
                    fp = item.data(0, Qt.ItemDataRole.UserRole)
                    if fp:
                        checked.append(fp)
            if not checked:
                QMessageBox.information(self, 'Nothing Selected', 'No files are checked.')
                return

            import time as _time
            ts = _time.strftime('%Y%m%d_%H%M%S')
            downloads = Path.home() / 'Downloads'
            dest = downloads / f'BaconSaver_restore_{ts}'

            exported = export_files(self._git_dir, self._current_hash, checked, dest)
            if exported:
                msg = QMessageBox(self)
                msg.setIcon(QMessageBox.Icon.Information)
                msg.setWindowTitle('Export Complete')
                msg.setText(f'Exported {len(exported)} file(s) to:\n{dest}')
                msg.addButton(QMessageBox.StandardButton.Ok)
                browse_btn = msg.addButton('Open Folder', QMessageBox.ButtonRole.ActionRole)
                msg.exec()
                if msg.clickedButton() == browse_btn:
                    os.startfile(str(dest))
            else:
                QMessageBox.warning(self, 'Export Failed', 'No files could be exported.')
        except Exception as e:
            QMessageBox.warning(self, 'Export Error', str(e))

    def _pick_font(self):
        font, ok = QFontDialog.getFont(self._preview_font, self)
        if ok:
            self._preview_font = font
            self._preview.setFont(font)
            self._save_preview_font()
            self._refresh_preview()

    @staticmethod
    def _load_preview_font() -> QFont:
        try:
            data = json.loads(CONFIG_FILE.read_text())
            f = data.get('restore_font', {})
            return QFont(f.get('family', 'Consolas'), f.get('size', 9))
        except Exception:
            return QFont('Consolas', 9)

    def _save_preview_font(self):
        try:
            data = json.loads(CONFIG_FILE.read_text())
        except Exception:
            data = {}
        data['restore_font'] = {
            'family': self._preview_font.family(),
            'size': self._preview_font.pointSize(),
        }
        CONFIG_FILE.write_text(json.dumps(data, indent=2))

    @staticmethod
    def _load_size() -> list[int]:
        try:
            data = json.loads(CONFIG_FILE.read_text())
            s = data.get('restore_size', [800, 600])
            return [int(s[0]), int(s[1])]
        except Exception:
            return [800, 600]

    def _save_size(self):
        try:
            data = json.loads(CONFIG_FILE.read_text())
        except Exception:
            data = {}
        data['restore_size'] = [self.width(), self.height()]
        CONFIG_FILE.write_text(json.dumps(data, indent=2))

    def closeEvent(self, event):
        self._save_size()
        event.accept()


# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('BaconSaver')
        self.setWindowIcon(QIcon(str(APP_DIR / 'bacon.svg')))
        self.resize(1000, 600)

        self._engines: dict[str, WatchEngine] = {}  # watch_path_str -> engine
        self._bridges: dict[str, _LogBridge] = {}
        self._shadows_base: Path | None = None

        # --- Central widget with splitter ---
        splitter = QSplitter(Qt.Orientation.Horizontal)

        # Left panel: directory list + buttons
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(4, 4, 4, 4)

        left_layout.addWidget(QLabel('Watched Directories'))
        self._dir_list = QListWidget()
        self._dir_list.setSelectionMode(QListWidget.SelectionMode.SingleSelection)
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
        restore_btn.clicked.connect(self._restore)
        left_layout.addWidget(restore_btn)
        self._restore_btn = restore_btn

        repo_btn = QPushButton('Set Repo Location...')
        repo_btn.clicked.connect(self._set_repo_location)
        left_layout.addWidget(repo_btn)

        splitter.addWidget(left)

        # Right panel: console
        self._console = QPlainTextEdit()
        self._console.setReadOnly(True)
        self._console.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
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

        _file_log('BaconSaver started')
        if self._shadows_base is not None:
            _file_log(f'Repo location: {self._shadows_base}')
        for path in self._engines:
            _file_log(f'Watching: {path}')

        # System tray
        self._tray = QSystemTrayIcon(self)
        self._tray.setIcon(QIcon(str(APP_DIR / 'bacon.svg')))
        self._tray.setToolTip('BaconSaver')
        tray_menu = QMenu()
        show_action = tray_menu.addAction('Show')
        show_action.triggered.connect(self._show_from_tray)
        tray_menu.addSeparator()
        quit_action = tray_menu.addAction('Quit')
        quit_action.triggered.connect(self._quit_app)
        self._tray.setContextMenu(tray_menu)
        self._tray.activated.connect(self._on_tray_activated)
        self._tray.show()

    # --- Logging ---

    def _make_log_callback(self, label: str) -> tuple[_LogBridge, callable]:
        bridge = _LogBridge()
        bridge.message.connect(lambda msg, lbl=label: self._append_log(lbl, msg))
        return bridge, lambda msg, b=bridge: b.message.emit(msg)

    def _append_log(self, label: str, msg: str):
        self._console.appendPlainText(f'[{label}] {msg}')
        _file_log(f'[{label}] {msg}')

    # --- Directory management ---

    def _add_directory(self):
        if self._shadows_base is None:
            reply = QMessageBox.question(
                self, 'Repo Location Required',
                'Set the repo location before adding directories.\n\n'
                'Set it now?',
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
            )
            if reply == QMessageBox.StandardButton.Yes:
                self._set_repo_location()
            if self._shadows_base is None:
                return
        dlg = AddDirectoryDialog(self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        resolved = dlg.get_path()
        if not resolved:
            return
        if resolved in self._engines:
            QMessageBox.information(self, 'Already Watching',
                                   f'{resolved} is already being watched.')
            return
        ver = git_version()
        if ver < (2, 37):
            reply = QMessageBox.question(
                self, 'Git Version Notice',
                f'Git version {".".join(str(v) for v in ver)} detected.\n\n'
                'File-system monitor (core.fsmonitor) requires Git 2.37 or newer.\n'
                'Without it, git status may be slower on very large directories.\n\n'
                'Proceed anyway?',
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
            )
            if reply != QMessageBox.StandardButton.Yes:
                return
        self._start_engine(resolved, initial_patterns=dlg.get_patterns())
        self._append_log('BaconSaver', f'Added: {resolved}')
        self._save_config()

    def _selected_item(self) -> QListWidgetItem | None:
        items = self._dir_list.selectedItems()
        return items[0] if items else None

    def _remove_directory(self):
        item = self._selected_item()
        if not item:
            return
        path = item.data(Qt.ItemDataRole.UserRole)
        reply = QMessageBox.question(
            self, 'Remove Directory',
            f'Stop watching {path}?\n\n'
            'History is preserved in the shadow repo and will be reused\n'
            'if you add this directory again.',
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
        )
        if reply != QMessageBox.StandardButton.Yes:
            return
        self._stop_engine(path)
        self._dir_list.takeItem(self._dir_list.row(item))
        self._append_log('BaconSaver', f'Removed: {path}')
        self._save_config()
        self._update_status()

    def _edit_ignores(self):
        item = self._selected_item()
        if not item:
            QMessageBox.information(self, 'No Selection',
                                   'Select a directory first.')
            return
        path = item.data(Qt.ItemDataRole.UserRole)
        engine = self._engines.get(path)
        if not engine:
            return
        dlg = IgnoreDialog(engine.ignore, self)
        if dlg.exec() == QDialog.DialogCode.Accepted:
            engine.ignore.set_patterns(dlg.get_patterns())
            engine.sync_exclude()
            self._append_log(Path(path).name, 'Ignore patterns updated.')

    def _toggle_pause(self):
        item = self._selected_item()
        if not item:
            return
        path = item.data(Qt.ItemDataRole.UserRole)
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

    def _restore(self):
        if self._shadows_base is None:
            return
        item = self._selected_item()
        if not item:
            QMessageBox.information(self, 'No Selection', 'Select a directory first.')
            return
        path = item.data(Qt.ItemDataRole.UserRole)
        shadow = self._shadows_base / shadow_name(path)
        git_dir = shadow / '.git'
        if not git_dir.exists():
            QMessageBox.warning(self, 'No History', 'No shadow repo found for this directory.')
            return
        dlg = RestoreDialog(path, shadow, self)
        dlg.exec()

    def _set_repo_location(self):
        current = str(self._shadows_base)
        path = QFileDialog.getExistingDirectory(self, 'Select Repo Location', current)
        if not path:
            return
        new_base = Path(path).resolve()
        if new_base == self._shadows_base:
            return
        if self._engines:
            reply = QMessageBox.question(
                self, 'Repo Location Changed',
                'Changing the repo location only affects newly added directories.\n'
                'Existing watched directories keep their current repos.\n\n'
                'Continue?',
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
            )
            if reply != QMessageBox.StandardButton.Yes:
                return
        self._shadows_base = new_base
        self._save_config()
        self._append_log('BaconSaver', f'Repo location set to: {new_base}')

        if not self._engines:
            self._status.showMessage(f'Repo location: {new_base}')

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
        item.setData(Qt.ItemDataRole.UserRole, path)
        self._dir_list.addItem(item)
        self._update_status()

    def _stop_engine(self, path: str):
        engine = self._engines.pop(path, None)
        if engine:
            engine.stop()
        self._bridges.pop(path, None)

    # --- Config persistence ---

    def _save_config(self):
        if self._shadows_base is not None:
            self._shadows_base.mkdir(parents=True, exist_ok=True)
        try:
            data = json.loads(CONFIG_FILE.read_text())
        except Exception:
            data = {}
        entries = []
        for path, engine in self._engines.items():
            entries.append({'path': path, 'paused': engine.is_paused})
        if self._shadows_base is not None:
            data['shadows_base'] = str(self._shadows_base)
        data['watched'] = entries
        CONFIG_FILE.write_text(json.dumps(data, indent=2))

    def _load_config(self):
        if not CONFIG_FILE.exists():
            return
        try:
            data = json.loads(CONFIG_FILE.read_text())
            sb = data.get('shadows_base')
            if sb:
                self._shadows_base = Path(sb)
            if self._shadows_base is None:
                return
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
                                if item.data(Qt.ItemDataRole.UserRole) == path:
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

    # --- First run ---

    def showEvent(self, event):
        super().showEvent(event)
        if not hasattr(self, '_first_run_checked'):
            self._first_run_checked = True
            if not CONFIG_FILE.exists():
                from PyQt6.QtCore import QTimer
                QTimer.singleShot(100, self._first_run_setup)

    def _first_run_setup(self):
        reply = QMessageBox.question(
            self, 'Welcome to BaconSaver',
            'No configuration found.\n\n'
            'Choose a location for the backup repository.\n'
            'This is where file history (git repos) will be stored.',
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
        )
        if reply == QMessageBox.StandardButton.Yes:
            self._set_repo_location()

    # --- Tray ---

    def _on_tray_activated(self, reason):
        if reason == QSystemTrayIcon.ActivationReason.DoubleClick:
            self._show_from_tray()

    def _show_from_tray(self):
        self.showNormal()
        self.activateWindow()
        self.raise_()

    def _quit_app(self):
        _file_log('BaconSaver shutting down')
        self._tray.hide()
        self._save_config()
        for path in list(self._engines):
            self._stop_engine(path)
        QApplication.quit()

    # --- Cleanup ---

    def changeEvent(self, event):
        if event.type() == QEvent.Type.WindowStateChange and self.isMinimized():
            self.hide()
            event.ignore()
            return
        super().changeEvent(event)

    def closeEvent(self, event):
        self._quit_app()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    import msvcrt
    lock_path = APP_DIR / '.lock'
    try:
        lock_file = open(lock_path, 'w')
        msvcrt.locking(lock_file.fileno(), msvcrt.LK_NBLCK, 1)
    except OSError:
        # Lock held by another process — already running
        from PyQt6.QtWidgets import QApplication, QMessageBox
        _app = QApplication(sys.argv)
        QMessageBox.warning(None, 'BaconSaver', 'BaconSaver is already running.')
        sys.exit(1)

    import atexit
    def _cleanup_lock():
        try:
            lock_file.close()
            lock_path.unlink(missing_ok=True)
        except Exception:
            pass
    atexit.register(_cleanup_lock)

    app = QApplication(sys.argv)
    app.setStyle('Fusion')
    window = MainWindow()
    window.show()
    sys.exit(app.exec())
