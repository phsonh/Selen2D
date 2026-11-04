"""PyQt6 development UI. Package operations reuse the command-line core."""
from __future__ import annotations
import codecs
from dataclasses import dataclass
import os
from pathlib import Path
import sys
import time
from PyQt6.QtCore import (Qt, QThread, pyqtSignal, QAbstractTableModel,
                         QModelIndex, QSortFilterProxyModel, QProcess)
from PyQt6.QtGui import QAction, QKeySequence
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
    QHBoxLayout, QFormLayout, QLineEdit, QPushButton, QFileDialog, QMessageBox,
    QLabel, QTabWidget, QGroupBox, QComboBox, QSpinBox, QPlainTextEdit,
    QProgressBar, QTableWidget, QTableWidgetItem, QTableView,
    QHeaderView, QAbstractItemView)
from .launch_common import load_launch_config, secret_bytes
from .resource_package import Package, pack, derive_key, OperationCancelled
from .tools_gui_editor import ConfigEditor, MigrationDeclined, load_project_config, save_config
from .tools_gui_process import BuildJob


@dataclass
class PackageRequest:
    operation: str
    source: str
    output: str = ''
    block_size: int = 262144
    level: int = 6
    encrypted: bool = False
    secret: bytes | None = None
    state: str = '等待'


class PackageWorker(QThread):
    progress = pyqtSignal(str, int)
    result = pyqtSignal(object)
    failure = pyqtSignal(str, bool)

    def __init__(self, request, parent=None):
        super().__init__(parent)
        self.request = request
        self.last_progress = 0.0
        self.last_phase = None

    def report(self, phase, done, total):
        now = time.monotonic()
        if phase != self.last_phase or now - self.last_progress > .05 or done == total:
            self.last_progress, self.last_phase = now, phase
            self.progress.emit(phase, int(done * 1000 / total) if total else -1)

    def run(self):
        request = self.request
        try:
            key = derive_key(request.secret) if request.secret is not None else None
            if request.operation == 'pack':
                packed = pack(request.source, request.output, block_size=request.block_size,
                    level=request.level, key=key if request.encrypted else None,
                    progress=self.report, cancelled=self.isInterruptionRequested)
                path = request.output
            else:
                packed, path = None, request.source
            self.report('index', 0, 0)
            with Package(path, key) as package:
                if self.isInterruptionRequested() and packed is None:
                    raise OperationCancelled('operation cancelled')
                if request.operation == 'verify':
                    package.verify(progress=self.report, cancelled=self.isInterruptionRequested)
                result = dict(header=dict(package.public_header), path=path,
                    index_authenticated=package.encrypted and package.entries is not None,
                    payload_verified=request.operation in ('pack', 'verify'),
                    files=None if package.entries is None else
                    [(e.path, '目录' if e.directory else '文件', e.size, len(e.blocks)) for e in package.entries])
                self.result.emit(result)
        except OperationCancelled as error:
            self.failure.emit(str(error), True)
        except Exception as error:
            self.failure.emit(str(error), False)


class FileListModel(QAbstractTableModel):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.rows = []

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.rows)

    def columnCount(self, parent=QModelIndex()):
        return 4

    def data(self, index, role=Qt.ItemDataRole.DisplayRole):
        if index.isValid() and role == Qt.ItemDataRole.DisplayRole:
            return self.rows[index.row()][index.column()]
        return None

    def headerData(self, section, orientation, role=Qt.ItemDataRole.DisplayRole):
        if orientation == Qt.Orientation.Horizontal and role == Qt.ItemDataRole.DisplayRole:
            return ['虚拟路径', '类型', '字节', '块数'][section]
        return super().headerData(section, orientation, role)

    def set_rows(self, rows):
        self.beginResetModel()
        self.rows = rows or []
        self.endResetModel()


class ToolsWindow(QMainWindow):
    def __init__(self, root=None):
        super().__init__()
        self.root = Path(root or Path(__file__).resolve().parents[3]).resolve()
        self.canonical = self.root / 'launch/launch.json'
        self.migration_backup = None
        config = load_project_config(self.root, self.confirm_base_migration,
                                     lambda backup: setattr(self, 'migration_backup', backup))
        self.dirty = False
        self.requests = []
        self.worker = None
        self.current_row = None
        self.build_process = None
        self.build_job = None
        self.build_cancelled = False
        self.close_pending = False
        self.redactions = set()
        self.built_package_keys = {}  # Session-only; never serialized or logged.
        self.setObjectName('selen2dTools')
        self.resize(940, 660)
        self.setMinimumSize(760, 560)
        self.editor = ConfigEditor()
        self.editor.changed.connect(self.edited)
        central = QWidget()
        layout = QVBoxLayout(central)
        self.config_status = QLabel()
        self.config_status.setTextFormat(Qt.TextFormat.PlainText)
        self.config_status.setWordWrap(True)
        layout.addWidget(self.config_status)
        self.tabs = QTabWidget()
        self.tabs.addTab(self.editor, '启动配置')
        self.tabs.addTab(self.make_packages(), '资源包')
        self.tabs.addTab(self.make_build(), '构建')
        layout.addWidget(self.tabs, 1)
        self.setCentralWidget(central)
        toolbar = self.addToolBar('配置')
        toolbar.setMovable(False)
        for text, handler, shortcut in [('保存配置', self.save, QKeySequence.StandardKey.Save),
                                        ('验证配置', self.validate, None)]:
            action = QAction(text, self)
            if shortcut is not None:
                action.setShortcut(shortcut)
            action.triggered.connect(lambda _, fn=handler: fn())
            toolbar.addAction(action)
        self.remember_secret(config)
        self.editor.set_config(config)
        self.refresh_status()
        if self.migration_backup is not None:
            self.statusBar().showMessage('目录类型已迁移；原配置私有备份：' + str(self.migration_backup))

    def confirm_base_migration(self, path):
        return QMessageBox.question(self, '启动配置需要迁移',
            '此 launch/launch.json 使用旧目录类型。\n'
            'deployment → root；user_data → local_app_data。\n'
            '确认后仅修改 base 字段，保留 secret、入口和挂载，并在 launch/ 内保存原文件备份。\n'
            '拒绝则保持文件不变并退出。现在迁移？',
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No) == QMessageBox.StandardButton.Yes

    def refresh_status(self):
        state = '有未保存修改' if self.dirty else '已保存'
        self.config_status.setText(f'项目配置：{self.canonical}\n{state} · 保存、打包和构建使用此文件')
        self.setWindowTitle('Selen2D Tools' + (' *' if self.dirty else ''))
        if hasattr(self, 'build_config'):
            self.build_config.setText(str(self.canonical))

    def edited(self):
        self.dirty = True
        self.refresh_status()

    def remember_secret(self, config):
        value = config.get('secret')
        if value:
            self.redactions.add(value)

    def redact(self, text):
        values = self.redactions | ({self.editor.secret.text()} if self.editor.secret.text() else set())
        for value in sorted(values, key=len, reverse=True):
            text = text.replace(value, '[secret]')
        return text

    def error(self, title, error):
        message = self.redact(str(error))
        self.statusBar().showMessage(title + '：' + message)
        QMessageBox.critical(self, title, message)

    def confirm_discard(self):
        if not self.dirty:
            return True
        result = QMessageBox.question(self, '未保存修改', '保存当前配置后继续？',
            QMessageBox.StandardButton.Save | QMessageBox.StandardButton.Discard | QMessageBox.StandardButton.Cancel,
            QMessageBox.StandardButton.Save)
        return self.save() if result == QMessageBox.StandardButton.Save else result == QMessageBox.StandardButton.Discard

    def save(self):
        try:
            if self.build_process is not None:
                raise ValueError('构建期间不能覆盖当前项目配置；请等待构建结束。')
            config = self.editor.get_config()
            write = True
            if self.has_package_work():
                try:
                    current = load_launch_config(self.canonical)
                except (OSError, ValueError):
                    current = None
                if config != current:
                    raise ValueError('资源包任务仍在运行或等待，不能更改当前项目配置。请先等待或安全取消任务。')
                write = False  # Identical configuration may enqueue another pack.
            self.remember_secret(config)
            if write:
                save_config(self.canonical, config)
            self.dirty = False
            self.refresh_status()
            self.statusBar().showMessage('配置已验证并保存：' + str(self.canonical))
            return True
        except (OSError, ValueError) as error:
            self.error('保存失败', error)
            return False

    def validate(self):
        try:
            self.editor.get_config()
        except ValueError as error:
            self.error('配置无效', error)
            return False
        QMessageBox.information(self, '配置有效',
            'JSON 结构、entry、parts、目录映射和加密规则有效。\n真实包内容、文件来源冲突仍由打包校验和引擎启动检查。')
        return True

    def path_row(self, edit, callback):
        row = QHBoxLayout()
        row.addWidget(edit)
        button = QPushButton('浏览…')
        button.clicked.connect(callback)
        row.addWidget(button)
        return row

    def make_packages(self):
        panel = QWidget()
        outer = QVBoxLayout(panel)
        box = QGroupBox('创建资源包 · 完成后自动校验所有块')
        form = QFormLayout(box)
        self.source = QLineEdit()
        self.source.setObjectName('packageSource')
        self.output = QLineEdit()
        self.output.setObjectName('packageOutput')
        form.addRow('资源输入目录', self.path_row(self.source, self.choose_source))
        form.addRow('输出包文件', self.path_row(self.output, self.choose_output))
        options = QHBoxLayout()
        self.version = QComboBox()
        self.version.addItems(['SLPK v1 · 明文', 'SLPK v2 · 使用当前配置 secret 加密'])
        self.block_size = QComboBox()
        for size in (65536, 131072, 262144, 524288, 1048576):
            self.block_size.addItem(f'{size // 1024} KiB', size)
        self.block_size.setCurrentIndex(2)
        self.level = QSpinBox()
        self.level.setRange(-131072, 22)
        self.level.setValue(6)
        options.addWidget(self.version, 2)
        options.addWidget(QLabel('块大小'))
        options.addWidget(self.block_size)
        options.addWidget(QLabel('Zstd 等级'))
        options.addWidget(self.level)
        form.addRow('格式与压缩', options)
        buttons = QHBoxLayout()
        self.pack_button = QPushButton('加入打包队列')
        self.pack_button.setObjectName('queuePack')
        self.pack_button.clicked.connect(self.queue_pack)
        inspect = QPushButton('检查已有包…')
        inspect.clicked.connect(lambda: self.choose_existing('inspect'))
        verify = QPushButton('完整校验已有包…')
        verify.clicked.connect(lambda: self.choose_existing('verify'))
        buttons.addWidget(self.pack_button)
        buttons.addWidget(inspect)
        buttons.addWidget(verify)
        form.addRow(buttons)
        outer.addWidget(box)
        self.queue = QTableWidget(0, 4)
        self.queue.setHorizontalHeaderLabels(['操作', '输入', '输出', '状态'])
        self.queue.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        self.queue.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeMode.Stretch)
        self.queue.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.queue.setMaximumHeight(110)
        outer.addWidget(self.queue)
        progress_row = QHBoxLayout()
        self.package_progress = QProgressBar()
        self.package_progress.setRange(0, 1000)
        self.package_phase = QLabel('就绪')
        self.cancel_packages = QPushButton('取消当前及等待任务')
        self.cancel_packages.clicked.connect(self.cancel_queue)
        progress_row.addWidget(self.package_phase)
        progress_row.addWidget(self.package_progress, 1)
        progress_row.addWidget(self.cancel_packages)
        outer.addLayout(progress_row)
        self.package_info = QLabel('选择“检查”读取索引；选择“完整校验”认证并读取全部数据块。')
        self.package_info.setTextFormat(Qt.TextFormat.PlainText)
        self.package_info.setWordWrap(True)
        outer.addWidget(self.package_info)
        search = QLineEdit()
        search.setPlaceholderText('筛选包内路径…')
        outer.addWidget(search)
        self.file_model = FileListModel(self)
        self.file_filter = QSortFilterProxyModel(self)
        self.file_filter.setSourceModel(self.file_model)
        self.file_filter.setFilterCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        self.file_filter.setFilterKeyColumn(0)
        search.textChanged.connect(self.file_filter.setFilterFixedString)
        self.files = QTableView()
        self.files.setModel(self.file_filter)
        self.files.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.files.setSortingEnabled(True)
        outer.addWidget(self.files, 1)
        self.package_log = QPlainTextEdit()
        self.package_log.setReadOnly(True)
        self.package_log.setMaximumBlockCount(5000)
        self.package_log.setMaximumHeight(80)
        outer.addWidget(self.package_log)
        return panel

    def choose_source(self):
        path = QFileDialog.getExistingDirectory(self, '资源输入目录', self.source.text() or str(self.root))
        if path:
            self.source.setText(path)

    def choose_output(self):
        path, _ = QFileDialog.getSaveFileName(self, '输出包文件', self.output.text() or str(self.root / 'game.slpk'),
                                             '资源包 (*.slpk);;所有文件 (*)')
        if path:
            self.output.setText(path)

    def package_secret(self, required=False):
        config = load_launch_config(self.canonical)
        self.remember_secret(config)
        return secret_bytes(config, required)

    def has_package_work(self):
        return self.worker is not None or any(request.state == '等待' for request in self.requests)

    def queue_pack(self):
        try:
            if self.build_process is not None:
                raise ValueError('构建期间不能启动打包任务。')
            encrypted = self.version.currentIndex() == 1
            source, output = self.source.text(), self.output.text()
            if not source or not Path(source).is_dir() or not output:
                raise ValueError('请选择存在的输入目录和输出文件。')
            if any(r.output and Path(r.output).resolve() == Path(output).resolve() and r.state in ('等待', '处理中') for r in self.requests):
                raise ValueError('该输出文件已经在队列中。')
            if Path(output).exists() and QMessageBox.question(self, '替换资源包',
                '完整校验成功后将原子替换：\n' + output,
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No) != QMessageBox.StandardButton.Yes:
                return
            if not self.save():
                return
            secret = self.package_secret(required=encrypted)
            self.enqueue(PackageRequest('pack', source, output, self.block_size.currentData(),
                                        self.level.value(), encrypted, secret if encrypted else None))
        except (OSError, ValueError) as error:
            self.error('无法加入任务', error)

    def choose_existing(self, operation):
        paths, _ = QFileDialog.getOpenFileNames(self, '选择资源包（可多选）', str(self.root),
                                                '资源包 (*.slpk);;所有文件 (*)')
        if paths:
            try:
                if self.build_process is not None:
                    raise ValueError('请等待构建结束。')
                if not self.save():
                    return
                secret = self.package_secret()
                for path in paths:
                    self.enqueue(PackageRequest(operation, path, secret=secret))
            except (OSError, ValueError) as error:
                self.error('无法加入任务', error)

    def enqueue(self, request):
        row = len(self.requests)
        self.requests.append(request)
        self.queue.insertRow(row)
        for col, value in enumerate((request.operation, request.source, request.output, request.state)):
            self.queue.setItem(row, col, QTableWidgetItem(value))
        self.start_next()

    def set_request_state(self, row, state):
        self.requests[row].state = state
        self.queue.item(row, 3).setText(state)

    def start_next(self):
        if self.worker is not None or self.close_pending:
            return
        row = next((i for i, request in enumerate(self.requests) if request.state == '等待'), None)
        if row is None:
            self.package_phase.setText('就绪')
            self.package_progress.setRange(0, 1000)
            return
        self.current_row = row
        self.set_request_state(row, '处理中')
        self.queue.selectRow(row)
        self.worker = PackageWorker(self.requests[row], self)
        self.worker.progress.connect(self.on_progress)
        self.worker.result.connect(self.on_package_result)
        self.worker.failure.connect(self.on_package_error)
        self.worker.finished.connect(self.on_package_finished)
        self.worker.start()

    def on_progress(self, phase, value):
        self.package_phase.setText({'scan': '扫描', 'pack': '打包', 'verify': '完整校验', 'index': '读取索引'}.get(phase, phase))
        self.package_progress.setRange(0, 0 if value < 0 else 1000)
        if value >= 0:
            self.package_progress.setValue(value)

    def on_package_result(self, result):
        self.set_request_state(self.current_row, '成功')
        header = result['header']
        request = self.requests[self.current_row]
        if request.operation == 'pack':
            output = str(Path(request.output).resolve())
            if header['encrypted']:
                self.built_package_keys[output] = derive_key(request.secret)
            else:
                self.built_package_keys.pop(output, None)
        authentication = ('索引已认证' if result['index_authenticated'] else '未认证，仅公开包头') if header['encrypted'] else '明文（无密码认证）'
        payload = '全部数据块校验通过' if result['payload_verified'] else '尚未校验数据块'
        self.package_info.setText(f"{result['path']}\nSLPK v{header['version']} · {header['entries']} 项 · "
            f"{header['physical_size']:,} 字节 · {authentication} · {payload}")
        self.file_model.set_rows(result['files'])
        self.package_log.appendPlainText(self.redact('成功：' + result['path'] + ' — ' + payload))
        if self.requests[self.current_row].operation == 'pack' and header['encrypted']:
            self.package_log.appendPlainText('加密包使用排队时已保存的项目配置；已有 EXE 必须与该配置重新构建。')
        self.statusBar().showMessage('资源包任务成功：' + payload)
        self.package_progress.setRange(0, 1000)
        self.package_progress.setValue(1000)

    def on_package_error(self, message, cancelled):
        self.set_request_state(self.current_row, '已取消' if cancelled else '失败')
        self.package_log.appendPlainText(self.redact(('已取消：' if cancelled else '失败：') + message))
        self.package_info.setText(self.redact(message))
        self.statusBar().showMessage(self.redact(('任务已取消：' if cancelled else '任务失败：') + message))

    def on_package_finished(self):
        self.requests[self.current_row].secret = None
        old, self.worker = self.worker, None
        old.deleteLater()
        if self.close_pending:
            self.close()
        else:
            self.start_next()

    def cancel_queue(self):
        for row, request in enumerate(self.requests):
            if request.state == '等待':
                self.set_request_state(row, '已取消')
                request.secret = None
        if self.worker is not None:
            self.worker.requestInterruption()
            self.package_phase.setText('正在安全取消…')

    def make_build(self):
        panel = QWidget()
        outer = QVBoxLayout(panel)
        info = QLabel('构建前验证并保存当前编辑内容到 launch/launch.json，然后调用仓库 tools/build.ps1。\n'
                      '终端无参数构建使用同一份配置；GUI 不维护隐藏的第二份构建配置。')
        info.setWordWrap(True)
        outer.addWidget(info)
        form = QFormLayout()
        self.build_config = QLabel(str(self.canonical))
        self.build_config.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        form.addRow('当前项目', self.build_config)
        self.configuration = QComboBox()
        self.configuration.addItems(['Debug', 'Release'])
        form.addRow('配置', self.configuration)
        outer.addLayout(form)
        buttons = QHBoxLayout()
        self.build_button = QPushButton('保存配置并构建')
        self.build_button.setObjectName('startBuild')
        self.build_button.clicked.connect(self.start_build)
        self.cancel_build_button = QPushButton('取消构建')
        self.cancel_build_button.setEnabled(False)
        self.cancel_build_button.clicked.connect(self.cancel_build)
        buttons.addWidget(self.build_button)
        buttons.addWidget(self.cancel_build_button)
        buttons.addStretch()
        outer.addLayout(buttons)
        self.build_state = QLabel('就绪。此操作只编译，不创建正式发行包。')
        outer.addWidget(self.build_state)
        self.build_log = QPlainTextEdit()
        self.build_log.setReadOnly(True)
        self.build_log.setMaximumBlockCount(20000)
        self.build_log.setObjectName('buildLog')
        outer.addWidget(self.build_log, 1)
        return panel

    def start_build(self):
        if self.build_process is not None:
            return
        if self.has_package_work():
            self.error('资源包任务未结束', '请等待包任务完成或安全取消后再构建。')
            return
        if os.name != 'nt':
            self.error('平台不支持', '构建按钮目前使用 Windows PowerShell / Visual Studio。CLI 打包和编辑不受此限制。')
            return
        try:
            config = self.editor.get_config()
            secret = secret_bytes(config)
            key = derive_key(secret) if secret is not None else None
            if any(value != key for value in self.built_package_keys.values()):
                if QMessageBox.question(self, '已有加密包使用另一份 secret',
                    '本次会话生成的部分加密包与当前配置的 secret 不同，不能和这次 EXE 共用。\n'
                    '请使用当前配置重新打包需要部署的资源。仍要继续构建 EXE 吗？',
                    QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                    QMessageBox.StandardButton.No) != QMessageBox.StandardButton.Yes:
                    return
        except ValueError as error:
            self.error('配置无效', error)
            return
        if not self.save():
            return
        process = QProcess(self)
        self.build_process = process
        self.build_cancelled = False
        self.build_decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')
        self.build_output_pending = ''
        self.build_log.clear()
        self.build_state.setText('正在启动受控构建进程…')
        self.build_button.setEnabled(False)
        self.cancel_build_button.setEnabled(True)
        # The helper is an internal package module, never a second public file
        # entry point. build.ps1 receives its absolute repository script path.
        process.setWorkingDirectory(str(self.root / 'tools/pack'))
        process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        process.started.connect(self.on_build_started)
        process.readyReadStandardOutput.connect(self.read_build_output)
        process.finished.connect(self.on_build_finished)
        process.errorOccurred.connect(self.on_build_process_error)
        script = self.root / 'tools/build.ps1'
        powershell = str(Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe')
        args = ['-u', '-m', 'selen2d_pytools.tools_gui_process', '--worker',
                powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script),
                '-Configuration', self.configuration.currentText()]
        self.build_log.appendPlainText(f'tools/build.ps1 -Configuration {self.configuration.currentText()}\n配置：{self.canonical}')
        process.start(sys.executable, args)

    def on_build_started(self):
        try:
            self.build_job = BuildJob(self.build_process.processId())
            self.build_process.write(b'go\n')
            self.build_process.closeWriteChannel()
            self.build_state.setText('正在构建；可安全取消本次进程树。')
        except OSError as error:
            self.build_log.appendPlainText('无法建立独立构建 Job，未启动编译：' + str(error))
            self.build_process.kill()  # Helper has not passed the gate; no children.

    def read_build_output(self):
        raw = bytes(self.build_process.readAllStandardOutput())
        self.append_build_output(self.build_decoder.decode(raw))

    def append_build_output(self, text, final=False):
        # Preserve a tail across QProcess chunks so a split secret cannot leak.
        self.build_output_pending += text
        values = self.redactions | ({self.editor.secret.text()} if self.editor.secret.text() else set())
        keep = max((len(value) - 1 for value in values), default=0)
        end = len(self.build_output_pending) if final else max(0, len(self.build_output_pending) - keep)
        if not final:
            for value in values:
                start = self.build_output_pending.rfind(value, 0, end + len(value))
                if start >= 0 and start < end < start + len(value):
                    end = start
        text, self.build_output_pending = self.build_output_pending[:end], self.build_output_pending[end:]
        self.build_log.moveCursor(self.build_log.textCursor().MoveOperation.End)
        self.build_log.insertPlainText(self.redact(text))

    def on_build_process_error(self, error):
        if error == QProcess.ProcessError.FailedToStart and self.build_process is not None:
            self.build_log.appendPlainText('启动失败：' + self.build_process.errorString())
            self.on_build_finished(-1, QProcess.ExitStatus.CrashExit)

    def on_build_finished(self, code, status):
        if self.build_process is None:
            return
        self.read_build_output()
        self.append_build_output(self.build_decoder.decode(b'', final=True), final=True)
        if self.build_job is not None:
            self.build_job.close()
            self.build_job = None
        old, self.build_process = self.build_process, None
        old.deleteLater()
        result = '已取消' if self.build_cancelled else '完成' if code == 0 and status == QProcess.ExitStatus.NormalExit else '失败'
        self.build_state.setText(f'构建{result} · 退出码 {code}')
        self.build_button.setEnabled(True)
        self.cancel_build_button.setEnabled(False)
        if self.close_pending:
            self.close()

    def cancel_build(self):
        if self.build_process is not None:
            self.build_cancelled = True
            self.build_state.setText('正在终止本次构建进程树…')
            try:
                if self.build_job is not None:
                    self.build_job.cancel()
                else:
                    self.build_process.kill()  # Pre-gate helper only.
            except OSError as error:
                self.error('取消失败', error)

    def closeEvent(self, event):
        if self.worker is not None or self.build_process is not None:
            if not self.close_pending and QMessageBox.question(self, '任务仍在运行',
                '安全取消当前任务后退出？现有资源包不会被未完成的输出替换。',
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No) != QMessageBox.StandardButton.Yes:
                event.ignore()
                return
            self.close_pending = True
            self.cancel_queue()
            self.cancel_build()
            event.ignore()
            return
        self.close_pending = False
        if self.confirm_discard():
            event.accept()
        else:
            event.ignore()


def run(argv):
    app = QApplication(argv)
    app.setApplicationName('Selen2D Tools')
    app.setOrganizationName('Lonely Owl')
    app.setStyle('Fusion')
    try:
        window = ToolsWindow()
    except MigrationDeclined:
        return 0
    except (OSError, ValueError) as error:
        QMessageBox.critical(None, '无法打开项目配置',
            'launch/launch.json 无法读取或初始化，未用默认配置覆盖已有文件。\n' + str(error))
        return 1
    window.show()
    return app.exec()
