# Selen2D Tools（PyQt6 开发工具）

此工具运行在开发机，编辑同一份启动 JSON、创建和检查 SLPK 包，以及调用现有构建脚本。它不进入游戏发行包；游戏运行不依赖 Python、PyQt6 或外部 JSON。

## 安装与启动

在仓库根目录执行，建议使用独立虚拟环境：

```powershell
python -m venv build/tools-venv
build/tools-venv/Scripts/python -m pip install -r tools/pack/requirements-gui.txt
build/tools-venv/Scripts/python -B tools/pack/main.py
```

安装完成后，也可以在 `tools/pack` 目录直接运行 `python main.py`，或在仓库根运行 `python tools/pack/main.py`。无参数 GUI 入口会优先使用本仓库已存在的 `build/tools-venv`，不向全局 Python 安装包，也不在启动时自动联网。若环境缺少依赖，会显示基于脚本位置的绝对路径安装命令，不受当前工作目录影响。CLI 与 `--help` 不会因这一 GUI 便利入口自动加载 Qt。

GUI 使用 PyQt6 6.11.0；普通引擎构建不需要这份 GUI/打包依赖。PyQt 的 GPL v3 / 商业许可及 Qt 的许可独立于引擎自身 MIT 许可证；本仓库不附带 PyQt 二进制，也不把它加入游戏发行包。依赖说明见 [Riverbank 官方许可说明](https://www.riverbankcomputing.com/software/pyqt/intro)。

## 唯一项目配置

GUI 只编辑本仓库的 `launch/launch.json`。工具栏只有“保存配置”（Ctrl+S）和“验证配置”，不提供新建、打开、另存为或多配置切换。

首次启动时，如果私有 JSON 不存在，工具严格验证 `src/res/development/launch.json` 后原子创建私有文件。默认入口是 `main.lua`，目录规则是 `deployment / . → .`，对应 EXE 所在目录。若私有文件已经存在但无效，工具显示错误并停止打开，保留原文件，不拿默认模板覆盖它。

可以修改 `entry`、资源包挂载与目录映射。实际目录的下拉显示为 `.（Root）` 和 `%APPDATA%（User Data）`，保存的 JSON `base` 仍分别是 `deployment` 与 `user_data`。这些只是界面标签；User Data 在 Windows 仍由系统 LocalAppData 解析，不改为 Roaming AppData，例如 `LonelyOwl/THKTNT` 映射到 `save`。`.` 是完整默认根映射的专用写法，不是允许任意路径穿越或扩大 Lua 文件 API。

标题中的星号表示未保存修改，关闭时会提醒保存。保存、包操作和构建均先使用同一验证器，再原子保存到唯一私有 JSON；失败不覆盖旧文件。“保存配置并构建”调用 `tools/build.ps1 -Configuration Debug` 或 `Release`，不传隐藏的配置路径。GUI 与终端无参数构建使用同一文件。

`secret` 默认隐藏，可明确切换显示；“配置包含 secret”区分省略 secret 和显式字符串。加密包使用已保存 JSON 的 secret 快照及与引擎编译相同的 SHA-256 派生；已有 EXE 仍须重新构建，工具不会自动修改已有 EXE。

资源包任务运行或等待期间不能保存不同配置或启动构建，相同配置可继续排队。构建期间也不能保存或打包。完成后的加密包若使用与当前配置不同的 secret，构建前明确提醒重新打包；这一比较只保存在本次 GUI 会话内，不生成第二种项目文件，不能替代部署验证。

GUI 不把 secret 作为命令行参数，不打印 JSON 内容，构建日志会遮蔽已知 secret。私有 JSON 保持在 Git 忽略的 `launch/` 内，不进入公共示例或发行目录。

配置校验覆盖结构、UTF-8/Unicode、entry 授权范围、重复 parts、目录别名冲突与加密入口规则。真实包文件与真实目录的完整路径冲突等来源检查仍由包校验及引擎启动执行，静态 JSON 校验不冒充运行时验证。

## 资源包

在“资源包”页选择输入目录和输出文件，设置 Zstd 等级与 64 KiB～1 MiB 的二次幂块大小。选择 SLPK v1 明文或 v2 加密后加入队列，可以连续提交多个不同输出。

`parts` 只存在于启动配置挂载表中，决定覆盖顺序，不会写进包的生成参数。打包使用与 CLI 相同的核心：稳定读取、分块编码、临时输出、完整校验、最后原子替换；已有输出会先请求确认。失败和取消不会用半成品覆盖旧包。

“检查已有包”读取包头与索引，“完整校验已有包”读取校验全部数据块，支持多选。结果区明确区分：

- 明文包：无密码认证，完整校验检查解码和 CRC。
- 加密包无 secret：仅公开包头，未认证，不展示可信文件列表。
- 加密包有 secret：索引认证通过后显示路径、类型、尺寸和块数；只有完整校验才报告全部载荷通过。

错误密钥、损坏内容或无效格式会显示失败，不报告成功。列表支持路径筛选与排序。进度是当前扫描/打包/校验阶段的进度，扫描和读取索引可能显示不定进度。

包任务在 QThread 中执行，界面持续响应。“取消当前及等待任务”在文件/块边界协作停止；正在处理单个压缩块或包索引时需等其返回。若取消恰好晚于原子发布，则任务可能已成功完成，界面如实显示结果。关闭窗口会先等待安全取消，不强行销毁仍在运行的工作线程。

## 构建与取消

构建按钮使用仓库已有 PowerShell/CMake 流程，需要 Visual Studio C++ 工具链。界面异步显示日志、退出码和结果，不创建正式发行包。公开发行仍使用独立的显式 `-PublicRelease` 构建和 Release-only 发布流程；GUI 私有配置不会隐式成为公共发行配置。

Windows 下每次构建都放入独立 Job Object。辅助进程在进入 Job 前不启动任何构建子进程；取消或 GUI 异常退出会终止该次受控进程树，不杀用户终端或其他构建。无法建立 Job 时，构建明确失败，不退化为无法安全取消的后台任务。取消可能留下正常的增量中间产物，下一次构建可继续。

目前构建按钮仅支持 Windows。JSON 编辑与包处理的实现可跨平台，但本轮实际验证以 Windows x64 为准。唯一公开 Python 入口为 `tools/pack/main.py`：无参数启动 GUI；`main.py launch build|inspect|verify` 使用启动配置 CLI；`main.py pack|inspect|verify` 使用资源包 CLI。`main.py --help` 仅需 Python 标准库，CI 的 launch 子命令不加载 GUI、压缩或加密依赖。实现放在内部 `selen2d_pytools/` 包，不能直接运行其中的文件。
