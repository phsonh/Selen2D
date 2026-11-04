# File、资源包与编译期启动配置

本指南描述 File/VFS、编译期启动配置与 Python 构建工具。当前开发构建面向 Windows x64；平台目录解析代码的存在不代表 macOS/Linux 已完成构建或运行验收。工具图形界面见 [Selen2D Tools](tools-gui.md)。

## 默认开发配置

源码开发者只编辑 JSON。CMake 自动校验并生成构建目录内的 `src/generated/launch.h`，将入口、资源包和目录授权编入 EXE。公开默认配置为 `src/res/development/launch.json`：

| 项目 | 默认值 |
|---|---|
| entry | `main.lua`，引导 `example/main.lua` |
| mounts | 空，没有资源包 |
| directories | 部署根 `root / . → .` |
| secret | 未提供 |

运行时不读外部 JSON 或启动头文件，也不能用命令行、CWD 或自定义环境变量替换规则。仅平台规定的用户数据位置按系统规则解析。授权项目目录不存在时不自动创建；Lua 可用 `File.CreateDirectory('save')` 创建所需子目录，`File.Open` 不自动建立父目录。

`source` 是相对 EXE 部署目录的物理包路径；`target` 是 Lua 看到的虚拟目录，两者可不同名。真实目录规则由 `{base,path,target}` 组成：`base` 选择 `root` 或 `local_app_data`，`path` 是基础根下非空项目子目录，`target` 是其虚拟别名。唯一根标记是完整记录 `{base="root",path=".",target="."}`，表示 EXE 部署根；其他位置的点、混合根记录和 local_app_data 根都无效。此规则不放宽 Lua 文件路径验证。字符串 `"output"` 等价于 `{ "base": "root", "path": "output", "target": "output" }`。

Windows 的 `local_app_data` 通过 Known Folder API 查询 LocalAppData，不把用户名或盘符写进配置。源码中的 macOS 实现使用用户域 Application Support，Linux 使用有效绝对 XDG_DATA_HOME 或 home 下的 `.local/share`；`local_app_data` 是跨平台的逻辑目录类型，不表示其他系统存在 Windows 环境变量；这些平台仍需独立验收。映射只授权最终项目子目录，不授权整个系统数据根；`userdata`、`save` 等名字没有额外权限。

## 路径、读写和错误

File 路径是非空 UTF-8/NFC 虚拟路径，使用 `/`，严格区分大小写。拒绝绝对路径、盘符、反斜杠、空组件、独立 `.`/`..`、末尾 `/`、NUL、控制字符与通配符。Unicode NFC 或大小写折叠后冲突的不同名字不能同时接入。只有 `require`/`package.searchpath` 的搜索模板候选允许先去掉开头的一个 `./`。

授权按完整路径组件匹配：`output` 不授权 `output2`。根映射授权全部安全部署路径；自定义非根映射的范围外查询也抛权限错误，不返回假“文件不存在”。范围内普通 I/O 失败返回独立 `{code, message}` 错误表；程序按 `code` 分支，诊断文本不作为稳定协议。完整签名见 [File API](../api/modules/File.lua)。

| 调用 | 成功或正常否定 | 普通 I/O 失败 |
|---|---|---|
| `Open`、`ListDirectory`、句柄 `Tell` | `value, nil` | `nil, err` |
| 修改函数、`RunLuaFile`、句柄 `Write/Seek/Close` | `true, nil` | `false, err` |
| `Exists/IsFile/IsDirectory` | `true, nil` 或正常 `false, nil` | `false, err` |
| `Read/ReadLine/ReadLines` | `data, true, nil` | `nil, false, err` |
| `GetScriptPath` | 来源字符串或单个 `nil` | 无可靠来源不算 I/O 失败 |

EOF 分别是 `Read` 的 `"", true, nil`、`ReadLine` 的 `nil, true, nil`、`ReadLines` 的 `{}, true, nil`。File 按原始字节读写，不隐式转换编码或换行。参数错误、关闭后使用、授权失败与异常 VFS 结构仍抛 Lua 错误。

```lua
local ok, err = File.CreateDirectory('output')
if not ok then error(err.message) end
local file, openError = File.Open('output/settings.txt', 'wb')
if not file then error(openError.message) end
local written, writeError = file:Write('volume=0.8\n')
local closed, closeError = file:Close()
if not written then error(writeError.message) end
if not closed then error(closeError.message) end
```

包内文件只读。`Copy` 可从包复制到获准真实目录中的新路径；不会借写模式在原虚拟路径建立覆盖副本。`Move/Replace` 要求同一实际文件系统，跨设备失败保留源。映射根与合并来源的目录不能作为普通目录整体搬移或卸载。根授权与 local_app_data→save 等另一真实源会形成重叠，不能组合；受保护游戏应改用明确的非根映射。包与真实文件同一完整虚拟路径仍报冲突。

## JSON 驱动的源码开发构建

普通源码构建需要 Python 3.10+ 和固定 Unicode 18.0.0 校验数据；不需要 PyQt6、PyNaCl 或 Zstandard。GUI/打包另有依赖清单。在源码根建立独立环境：

```powershell
python -m venv build/tools-venv
.\build\tools-venv\Scripts\python.exe -m pip install -r tools/pack/requirements-launch.txt
# 如需 GUI 和打包，改为安装 requirements-gui.txt（包含全部工具依赖）
```

GUI 只编辑被忽略的 `launch/launch.json`。不存在时由公开默认模板初始化；已有无效文件会报错，不会用默认值覆盖。界面只有保存配置（Ctrl+S）和验证配置，不提供新建、打开、另存为或当前项目切换。保存后直接构建，无需手工生成或编辑头文件。

```json
{
  "mounts": [],
  "directories": [{"base": "root", "path": ".", "target": "."}],
  "entry": "main.lua"
}
```

```powershell
.\tools\build.ps1 -Configuration Release
.\tools\build.ps1 -Configuration Debug
```

普通 build.ps1 每次重新选择并输出路径：存在的唯一私有 `launch/launch.json`，否则公开默认 JSON。不会把上次 CMake cache 当成当前项目。脚本优先使用 `build/tools-venv/Scripts/python.exe`，也可用 `-Python` 指定解释器。私有 `launch/` 受 Git 忽略；不要把 secret 配置放到资源输入树或发行目录。

仅 CI 的显式配置入口仍可直接指定 JSON；它不构成 GUI 的第二份活动配置，普通 build.ps1 会清除旧缓存选择：

```powershell
cmake --preset selen2d-release -DENGINE_LAUNCH_CONFIG=launch/launch.json -DENGINE_LAUNCH_HEADER= -DPython3_EXECUTABLE=build/tools-venv/Scripts/python.exe
cmake --build --preset selen2d-release
```

JSON、校验器和 Unicode 表均属于生成依赖。修改 JSON 后，增量构建重新校验、生成头文件、编译 Startup.cpp 并链接；生成失败中止构建，旧产物不会被当作本次成功结果。生成器原子替换头文件，文件只位于 build 目录。运行时不会读取 JSON 或头文件。配置校验不会冒充部署文件存在性或包认证检查，这些仍在打包验证与引擎启动时执行。

静态 DXC 使用各构建目录自己的缓存，Debug 与 Release 不共享产物。普通 JSON 或引擎源码修改不会重编 DXC；固定源码、集成脚本或工具链变更则进入其子构建更新。每轮只做轻量的库完整性检查，库变化时才校验摘要，缺失或损坏时重建。`build.ps1 -DxcJobs 1` 可降低 LLVM 编译内存压力；默认 4，可选 1～4，与普通引擎的 `-Jobs` 分开。首次编译仍从仓库内固定源码构建，不下载或部署 DXC DLL。

仅为已有自动化保留显式 `ENGINE_LAUNCH_HEADER`；它与 `ENGINE_LAUNCH_CONFIG` 同时非空时配置失败。build.ps1 始终清空旧头文件入口，GUI 不提供手写头文件选项。直接运行 CMake 会遵循自己显式设置的 cache；恢复自动选择须清空两个变量。

## 旧配置迁移

目录类型只接受 `root` 和 `local_app_data`。旧开发配置中的 `deployment` / `user_data` 不再被构建校验器接受；CLI 会明确报告需要迁移，不会静默选用默认配置。

打开 GUI 后，若唯一私有 `launch/launch.json` 使用旧名称，会先询问是否迁移。确认后只转换这两种 `base` 值，保留 secret、挂载、入口及其他字段；验证成功后备份原文件并原子保存。拒绝或校验失败不会覆盖原配置。备份同样含私有数据，留在被忽略的 `launch/` 下，不加入仓库或发行包。新保存的 JSON 一律使用新名称。

`root` 是 EXE 目录，不是磁盘根。`local_app_data` 在 Windows 使用系统 Known Folder LocalAppData（`%LOCALAPPDATA%`），不是 Roaming `%APPDATA%`。`.` 只对完整的 `root / . → .` 目录声明有效。

## 与标准 Release 隔离

正式通用包必须使用独立 `build/public-release`，强制选择公开默认 JSON，即使私有 launch/launch.json 存在也不会采用。标准配置还验证无 secret、无资源包且保持根 main.lua 入口与 root 根映射。

```powershell
.\tools\build.ps1 -Configuration Release -PublicRelease -StageRelease
# 等价的显式发布目标，须先配置/构建上述公开目录：
cmake --build build/public-release --target stage_release
```

普通 build/release 或任何 Debug 构建的 stage_release 明确失败；不会复制自定义 EXE。标准包只包含 EXE、根 main.lua 引导、完整 example/ 示例、许可与 doc/api、doc/guide，不包含 JSON、头文件、工具、Python 或私有 secret。默认示例 Lua/HLSL 修改后重启即可。自定义游戏请自行将编译所得 EXE 与对应包放到部署目录，勿调用标准发布目标替代游戏部署。

修改 entry、mounts、directories 或 secret 都需要重新构建 EXE。改变 secret 还需用同一配置重打加密包。CLI main.py launch 的 inspect/build/verify 继续供 CI 使用，GUI 和 build.ps1 用户无需手工调用；inspect 不显示 secret 或摘要，verify 不执行任意 C++。

## 打包、检查与部署

`tools/pack/main.py` 的 pack/inspect/verify 子命令 支持明文 SLPK v1 与认证加密 SLPK v2。输入是稳定的普通物理目录；包内路径相对该目录，不包含目录名或挂载 target。文件扩展名不参与格式识别，ZIP 不是本工具的输入或输出格式。

假设独立游戏输入为 `../MyGame/content/scripts/main.lua` 等资源，部署目录也在游戏项目中：

```powershell
New-Item -ItemType Directory -Force ../MyGame/dist
.\build\tools-venv\Scripts\python.exe tools/pack/main.py pack --source ../MyGame/content --output ../MyGame/dist/data.bin
.\build\tools-venv\Scripts\python.exe tools/pack/main.py inspect --input ../MyGame/dist/data.bin
.\build\tools-venv\Scripts\python.exe tools/pack/main.py verify --input ../MyGame/dist/data.bin
```

默认块大小为 262144 字节、Zstd 等级为 6；`--block-size` 支持 65536～1048576 的二次幂，`--level` 接受工具所用 Zstd 支持的范围。每块独立压缩，压缩不变小时存原始数据；空文件和空目录保留。输入树中的重定向链接、NFC/大小写冲突和源树内的输出路径会被拒绝。工具不会默认忽略 `.git` 或临时文件，应只传干净的资源目录。

将明文包接入时，编译配置中的记录为 `source="data.bin"`、`target="data"`、唯一 `parts` 与 `encrypted=false`，入口可写 `data/scripts/main.lua`。包记录按 parts 升序处理；较大 parts 覆盖包间同路径文件。同一 target 可有多个包，parts 必须全局唯一。真实目录不能覆盖包文件：任意包与真实来源同虚拟路径文件共存都会报冲突，即使那个包文件已被另一包遮蔽。

加密构建可使用下面的私有 `launch/launch.json`；示例字符串只说明字段，项目应自行设定实际字符串：

```json
{
  "secret": "replace-with-your-project-string",
  "mounts": [
    {"source": "data.bin", "target": "data", "parts": 0, "encrypted": true}
  ],
  "directories": [
    {"base": "local_app_data", "path": "ExampleStudio/ExampleGame", "target": "profile"}
  ],
  "entry": "data/scripts/main.lua"
}
```

```powershell
.\build\tools-venv\Scripts\python.exe tools/pack/main.py pack --source ../MyGame/content --output ../MyGame/dist/data.bin --encrypt --config launch/launch.json
.\build\tools-venv\Scripts\python.exe tools/pack/main.py inspect --input ../MyGame/dist/data.bin --config launch/launch.json
.\build\tools-venv\Scripts\python.exe tools/pack/main.py verify --input ../MyGame/dist/data.bin --config launch/launch.json
```

通过 GUI 保存唯一私有配置后直接执行 build.ps1；GUI 加密打包复用其中同一份 secret。将所得 EXE 与 `data.bin` 放入匹配的部署目录，并保留项目和第三方许可；不要部署 JSON、生成头文件或 Python 环境。

密钥为一次 `SHA256(UTF8(secret))` 的 32 字节原始结果。空格、换行、NUL 和 NFC/NFD 差异全部保留，不 trim、不规范化、不追加 C 字符串终止符。显式空字符串可表达，缺少 secret 则不能请求加密。v2 使用 XChaCha20-Poly1305-IETF 认证索引和独立数据块，每次生产打包使用新的随机身份与 nonce；相同输入不保证相同密文。

不带配置的 v2 `inspect` 只报告未认证公开包头。带正确配置的 `inspect` 验证索引并列条目，仍不代表所有数据块完整；`verify` 才逐块验证。运行时挂载先验证索引，读取时只认证/解压所需块，不把整包明文解到临时目录。错误密钥、版本或加密要求不匹配、损坏内容不会降级为明文或回退旧包。

## 脚本入口与能力边界

只要配置中存在加密挂载，entry、`require` 文件搜索器、`File.RunLuaFile`、带文件名的 `loadfile/dofile` 就只接受通过认证的加密包内代码。真实目录仍可读写数据；较早搜索模板命中真实 Lua 文件会报来源错误，不悄悄跳过它。`package.loaded`、preload 与应用自行安装的搜索器保持 Lua 机制。

标准 `io` 的文件名入口、默认受控句柄、`os.remove/rename` 和 `package.searchpath` 共用 VFS 授权，但保留 LuaJIT 的参数和失败返回形式，不返回 File 专用错误表。没有原生库授权名单，`package.loadlib` 和磁盘 C 模块搜索器明确拒绝；自动系统临时文件入口 `io.tmpfile/os.tmpname` 也明确拒绝。

宿主 stdio、`io.popen/os.execute`、FFI、`load/loadstring` 与无文件名的 stdin 代码加载仍按实际 LuaJIT 能力暴露。这些能力不构成完整不可信脚本沙箱，字符串或流不会因此被标为已认证包代码。认证加密的目标是提高直接解包与简单替换的门槛，不提供反调试、签名发行者证明或防止持密钥者重打包的保证。

Shader 示例继续使用内建 `Engine.hlsl` 与现有渲染接口，见 [Shader 指南](shader.md)。File 与包的 CPU/互操作结果不能代替 Resource、Graphic、Audio 的组合测试或 GPU、音频设备验收。
