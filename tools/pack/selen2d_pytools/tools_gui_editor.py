"""Widgets for the existing launch JSON schema; no alternative config format."""
from __future__ import annotations
import json
import os
from pathlib import Path
import tempfile
from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QFormLayout,
    QLineEdit, QCheckBox, QPushButton, QTableWidget, QTableWidgetItem,
    QComboBox, QLabel, QHeaderView, QAbstractItemView, QGroupBox)
from .launch_common import ConfigError, load_launch_config, validate_launch


def save_config(path, config, *, replace_existing=True):
    """Validate before atomic replacement; never leave a half-written secret."""
    normalized = validate_launch(config)
    encoded = (json.dumps(normalized, ensure_ascii=False, indent=2) + '\n').encode('utf-8')
    if len(encoded) > 8 * 1024 * 1024:
        raise ConfigError('configuration exceeds 8 MiB')
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        fd, name = tempfile.mkstemp(prefix='.' + path.name + '.', suffix='.tmp', dir=path.parent)
        temporary = Path(name)
        with os.fdopen(fd, 'wb') as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        if replace_existing:
            os.replace(temporary, path)
        else:
            # Publish only when absent. If another editor created the file,
            # never overwrite its configuration, valid or otherwise.
            os.link(temporary, path)
            temporary.unlink()
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return normalized


def load_project_config(root):
    path = Path(root) / 'launch/launch.json'
    if not path.exists():
        template = load_launch_config(Path(root) / 'src/res/development/launch.json')
        try:
            save_config(path, template, replace_existing=False)
        except FileExistsError:
            pass  # Another process won creation; validate its file below.
    return load_launch_config(path)


class ConfigEditor(QWidget):
    changed = pyqtSignal()

    def __init__(self):
        super().__init__()
        self.loading = False
        outer = QVBoxLayout(self)
        intro = QLabel('直接编辑引擎使用的 launch.json。parts 仅控制挂载覆盖顺序，不属于资源包格式参数。')
        intro.setWordWrap(True)
        outer.addWidget(intro)
        form = QFormLayout()
        self.entry = QLineEdit()
        self.entry.setObjectName('entry')
        self.entry.setPlaceholderText('main.lua')
        self.entry.textChanged.connect(self._changed)
        form.addRow('入口 entry', self.entry)
        secret_row = QHBoxLayout()
        self.has_secret = QCheckBox('配置包含 secret')
        self.has_secret.setObjectName('hasSecret')
        self.secret = QLineEdit()
        # QLineEdit otherwise silently truncates valid imported long secrets.
        self.secret.setMaxLength(8 * 1024 * 1024)
        self.secret.setObjectName('secret')
        self.secret.setEchoMode(QLineEdit.EchoMode.Password)
        self.reveal = QCheckBox('显示')
        self.reveal.toggled.connect(lambda checked: self.secret.setEchoMode(
            QLineEdit.EchoMode.Normal if checked else QLineEdit.EchoMode.Password))
        self.has_secret.toggled.connect(self.secret.setEnabled)
        self.has_secret.toggled.connect(self._changed)
        self.secret.textChanged.connect(self._changed)
        secret_row.addWidget(self.has_secret)
        secret_row.addWidget(self.secret, 1)
        secret_row.addWidget(self.reveal)
        form.addRow('加密密钥来源', secret_row)
        outer.addLayout(form)
        self.mounts = self._table(['source', 'target', 'parts', 'encrypted'], 'mounts')
        self.directories = self._table(['base', 'path', 'target'], 'directories')
        for title, table, add in [('资源包挂载 mounts', self.mounts, self.add_mount),
                                  ('目录映射 directories', self.directories, self.add_directory)]:
            box = QGroupBox(title)
            layout = QVBoxLayout(box)
            layout.addWidget(table)
            buttons = QHBoxLayout()
            create = QPushButton('添加')
            create.clicked.connect(lambda _, action=add: action())
            remove = QPushButton('删除选中行')
            remove.clicked.connect(lambda _, target=table: self.remove_rows(target))
            buttons.addWidget(create)
            buttons.addWidget(remove)
            buttons.addStretch()
            layout.addLayout(buttons)
            outer.addWidget(box)
        note = QLabel('deployment：EXE 所在目录下的相对路径。user_data：Windows LocalAppData 下的相对路径。\n'
                      '例如 user_data / LonelyOwl/THKTNT → save，Lua 通过 save/... 访问。secret 只保存在私有 JSON 中。')
        note.setWordWrap(True)
        outer.addWidget(note)
        outer.addStretch(1)

    def _table(self, headers, name):
        table = QTableWidget(0, len(headers))
        table.setObjectName(name)
        table.setHorizontalHeaderLabels(headers)
        table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        table.verticalHeader().setDefaultSectionSize(26)
        table.itemChanged.connect(self._changed)
        self.resize_table(table)
        return table

    @staticmethod
    def resize_table(table):
        # Keep empty/small lists compact, with scrolling once four rows fill.
        rows = max(1, min(table.rowCount(), 4))
        table.setFixedHeight(34 + rows * 26)

    def _changed(self, *_):
        if not self.loading:
            self.changed.emit()

    def add_mount(self, record=None):
        record = record or dict(source='resources.slpk', target='game',
            parts=self.mounts.rowCount(), encrypted=False)
        row = self.mounts.rowCount()
        self.mounts.insertRow(row)
        for col, name in enumerate(('source', 'target', 'parts')):
            self.mounts.setItem(row, col, QTableWidgetItem(str(record[name])))
        check = QCheckBox('v2 加密')
        check.setChecked(record['encrypted'])
        check.toggled.connect(self._changed)
        self.mounts.setCellWidget(row, 3, check)
        self.resize_table(self.mounts)
        self._changed()

    def add_directory(self, record=None):
        record = record or dict(base='deployment', path='.', target='.')
        if isinstance(record, str):
            record = dict(base='deployment', path=record, target=record)
        row = self.directories.rowCount()
        self.directories.insertRow(row)
        combo = QComboBox()
        combo.addItem('.（Root）', 'deployment')
        combo.addItem('%APPDATA%（User Data）', 'user_data')
        combo.setCurrentIndex(combo.findData(record['base']))
        combo.currentIndexChanged.connect(self._changed)
        self.directories.setCellWidget(row, 0, combo)
        for col, name in ((1, 'path'), (2, 'target')):
            self.directories.setItem(row, col, QTableWidgetItem(record[name]))
        self.resize_table(self.directories)
        self._changed()

    def remove_rows(self, table):
        for row in sorted({index.row() for index in table.selectedIndexes()}, reverse=True):
            table.removeRow(row)
        self.resize_table(table)
        self._changed()

    def set_config(self, config):
        self.loading = True
        try:
            self.entry.setText(config['entry'])
            self.has_secret.setChecked('secret' in config)
            self.secret.setEnabled('secret' in config)
            self.secret.setText(config.get('secret', ''))
            self.reveal.setChecked(False)
            self.mounts.setRowCount(0)
            self.directories.setRowCount(0)
            for row in config['mounts']:
                self.add_mount(row)
            for row in config['directories']:
                self.add_directory(row)
            self.resize_table(self.mounts)
            self.resize_table(self.directories)
        finally:
            self.loading = False

    @staticmethod
    def _text(table, row, col):
        item = table.item(row, col)
        return item.text() if item else ''

    def get_config(self):
        config = dict(entry=self.entry.text(), mounts=[], directories=[])
        if self.has_secret.isChecked():
            config['secret'] = self.secret.text()
        for row in range(self.mounts.rowCount()):
            token = self._text(self.mounts, row, 2)
            if not token.isascii() or not token.isdecimal():
                raise ConfigError(f'mounts 第 {row + 1} 行 parts 必须为 uint32 整数')
            config['mounts'].append(dict(source=self._text(self.mounts, row, 0),
                target=self._text(self.mounts, row, 1), parts=int(token),
                encrypted=self.mounts.cellWidget(row, 3).isChecked()))
        for row in range(self.directories.rowCount()):
            config['directories'].append(dict(base=self.directories.cellWidget(row, 0).currentData(),
                path=self._text(self.directories, row, 1), target=self._text(self.directories, row, 2)))
        return validate_launch(config)
