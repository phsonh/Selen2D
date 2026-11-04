# File、资源包与编译期启动配置

本指南描述当前阶段 C 的 File/VFS、启动配置与 Python 构建工具。当前发布构建面向 Windows x64；平台目录解析代码的存在不代表 macOS/Linux 已完成构建或运行验收。Resource、Graphic、Audio 的后续二号机接口迁移不在本指南的完成声明内。

## 默认开发配置

EXE 内的 `launch.h` 决定入口、资源包和真实目录授权。默认使用源码 `src/res/development/launch.h.in`：

| 项目 | 默认值 |
|---|---|
| entry | `example/main.lua` |
| mounts | 空，没有资源包 |
| directories | 部署目录 `example → example`、`output → output` |
| secret | 未提供 |

运行时不读外部启动文件，也不能用命令行、CWD 或自定义环境变量替换规则。仅平台规定的用户数据位置按系统规则解析。授权项目目录不存在时不自动创建；Lua 可用 `File.CreateDirectory('output')` 创建默认写入根，`File.Open` 不自动建立父目录。

`source` 是相对 EXE 部署目录的物理包路径；`target` 是 Lua 看到的虚拟目录，两者可不同名。真实目录规则由 `{base,path,target}` 组成：`base` 选择 `deployment` 或 `user_data`，`path` 是基础根下非空项目子目录，`target` 是其虚拟别名。字符串 `"output"` 等价于 `{ "base": "deployment", "path": "output", "target": "output" }`。

Windows 的 `user_data` 通过 Known Folder API 查询 LocalAppData，不把用户名或盘符写进配置。源码中的 macOS 实现使用用户域 Application Support，Linux 使用有效绝对 XDG_DATA_HOME 或 home 下的 `.local/share`；这些平台仍需独立验收。映射只授权最终项目子目录，不授权整个系统数据根；`userdata`、`save` 等名字没有额外权限。

## 路径、读写和错误

File 路径是非空 UTF-8/NFC 虚拟路径，使用 `/`，严格区分大小写。拒绝绝对路径、盘符、反斜杠、空组件、独立 `.`/`..`、末尾 `/`、NUL、控制字符与通配符。Unicode NFC 或大小写折叠后冲突的不同名字不能同时接入。只有 `require`/`package.searchpath` 的搜索模板候选允许先去掉开头的一个 `./`。

授权按完整路径组件匹配：`output` 不授权 `output2`。范围外查询也抛权限错误，不返回假“文件不存在”。范围内普通 I/O 失败返回独立 `{code, message}` 错误表；程序按 `code` 分支，诊断文本不作为稳定协议。完整签名见 [File API](../api/modules/File.lua)。

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

包内文件只读。`Copy` 可从包复制到获准真实目录中的新路径；不会借写模式在原虚拟路径建立覆盖副本。`Move/Replace` 要求同一实际文件系统，跨设备失败保留源。映射根与合并来源的目录不能作为普通目录整体搬移或卸载。

## 生成或手写启动配置

Python 工具只在构建时使用；默认 CMake 构建不需要安装这些 Python 包。需要生成配置或打包时，在源码根创建独立环境：

```powershell
python -m venv build/tools-venv
.\build\tools-venv\Scripts\python.exe -m pip install -r tools/requirements-tools.txt
New-Item -ItemType Directory -Force launch, build/launch
```

把以下开发配置保存为 `launch/game.source.json`。`launch/` 是当前仓库忽略的本机构建目录；不要将真实秘密配置放进资源输入树。

```json
{
  "mounts": [],
  "directories": ["example", "output"],
  "entry": "example/main.lua"
}
```

```powershell
.\build\tools-venv\Scripts\python.exe tools/make_launch.py build --config launch/game.source.json --output build/launch/launch.h
.\build\tools-venv\Scripts\python.exe tools/make_launch.py inspect --config launch/game.source.json
.\build\tools-venv\Scripts\python.exe tools/make_launch.py verify --config launch/game.source.json --header build/launch/launch.h
```

`build` 在目标旁生成临时文件，成功后替换输出；输出父目录须已存在。`inspect` 不显示 secret 或摘要。`verify` 比较配置与固定生成内容，不执行 C++，不解析任意手写头文件。这三条命令只校验配置，不访问部署树、验证包内容或运行引擎。

在已配置 MSVC 14.44、CMake 和 Ninja 的 x64 开发者终端选择这份头文件：

```powershell
cmake --preset selen2d-release -DENGINE_LAUNCH_HEADER=build/launch/launch.h
cmake --build --preset selen2d-release
```

每次构建只选一个来源，CMake 复制它到构建目录供专用启动编译单元包含，不改写所选原文件。也可手写等价头文件，参照 `src/engine/LaunchConfig.h` 与默认模板声明 `engine::launch_config::value`；手写和生成配置都会经过宿主的路径、映射、冲突与代码来源检查。secret 使用明确长度的 UTF-8 字节数组，区分“未提供”和“显式空字符串”。

选择会保存在该构建目录的 CMake cache 中。恢复默认配置时明确清空，然后重新构建：

```powershell
cmake --preset selen2d-release -DENGINE_LAUNCH_HEADER=
cmake --build --preset selen2d-release
```

修改 entry、mounts、directories 或 secret 都需要重新构建 EXE。改变 secret 还需要重打加密包。`stage_release` 仍只收集固定开发示例和文档，不根据私有配置自动收集游戏包；自定义部署应按自己的编译记录放入资源并实际启动验证。

## 打包、检查与部署

`tools/pack_resources.py` 支持明文 SLPK v1 与认证加密 SLPK v2。输入是稳定的普通物理目录；包内路径相对该目录，不包含目录名或挂载 target。文件扩展名不参与格式识别，ZIP 不是本工具的输入或输出格式。

假设游戏输入为 `assets/scripts/main.lua` 等资源，下面的 `build/game-deploy/` 是另行准备的部署目录：

```powershell
New-Item -ItemType Directory -Force build/game-deploy
.\build\tools-venv\Scripts\python.exe tools/pack_resources.py pack --source assets --output build/game-deploy/data.bin
.\build\tools-venv\Scripts\python.exe tools/pack_resources.py inspect --input build/game-deploy/data.bin
.\build\tools-venv\Scripts\python.exe tools/pack_resources.py verify --input build/game-deploy/data.bin
```

默认块大小为 262144 字节、Zstd 等级为 6；`--block-size` 支持 65536～1048576 的二次幂，`--level` 接受工具所用 Zstd 支持的范围。每块独立压缩，压缩不变小时存原始数据；空文件和空目录保留。输入树中的重定向链接、NFC/大小写冲突和源树内的输出路径会被拒绝。工具不会默认忽略 `.git` 或临时文件，应只传干净的资源目录。

将明文包接入时，编译配置中的记录为 `source="data.bin"`、`target="data"`、唯一 `parts` 与 `encrypted=false`，入口可写 `data/scripts/main.lua`。包记录按 parts 升序处理；较大 parts 覆盖包间同路径文件。同一 target 可有多个包，parts 必须全局唯一。真实目录不能覆盖包文件：任意包与真实来源同虚拟路径文件共存都会报冲突，即使那个包文件已被另一包遮蔽。

加密构建可使用下面的私有 `launch/game.source.json`；示例字符串只说明字段，项目应自行设定实际字符串：

```json
{
  "secret": "replace-with-your-project-string",
  "mounts": [
    {"source": "data.bin", "target": "data", "parts": 0, "encrypted": true}
  ],
  "directories": [
    {"base": "user_data", "path": "ExampleStudio/ExampleGame", "target": "profile"}
  ],
  "entry": "data/scripts/main.lua"
}
```

```powershell
.\build\tools-venv\Scripts\python.exe tools/pack_resources.py pack --source assets --output build/game-deploy/data.bin --encrypt --config launch/game.source.json
.\build\tools-venv\Scripts\python.exe tools/pack_resources.py inspect --input build/game-deploy/data.bin --config launch/game.source.json
.\build\tools-venv\Scripts\python.exe tools/pack_resources.py verify --input build/game-deploy/data.bin --config launch/game.source.json
```

再用同一配置生成并编译 `launch.h`。将所得 EXE 与 `data.bin` 放入匹配的部署目录，并保留项目和第三方许可；不要部署 JSON、生成头文件或 Python 环境。

密钥为一次 `SHA256(UTF8(secret))` 的 32 字节原始结果。空格、换行、NUL 和 NFC/NFD 差异全部保留，不 trim、不规范化、不追加 C 字符串终止符。显式空字符串可表达，缺少 secret 则不能请求加密。v2 使用 XChaCha20-Poly1305-IETF 认证索引和独立数据块，每次生产打包使用新的随机身份与 nonce；相同输入不保证相同密文。

不带配置的 v2 `inspect` 只报告未认证公开包头。带正确配置的 `inspect` 验证索引并列条目，仍不代表所有数据块完整；`verify` 才逐块验证。运行时挂载先验证索引，读取时只认证/解压所需块，不把整包明文解到临时目录。错误密钥、版本或加密要求不匹配、损坏内容不会降级为明文或回退旧包。

## 脚本入口与能力边界

只要配置中存在加密挂载，entry、`require` 文件搜索器、`File.RunLuaFile`、带文件名的 `loadfile/dofile` 就只接受通过认证的加密包内代码。真实目录仍可读写数据；较早搜索模板命中真实 Lua 文件会报来源错误，不悄悄跳过它。`package.loaded`、preload 与应用自行安装的搜索器保持 Lua 机制。

标准 `io` 的文件名入口、默认受控句柄、`os.remove/rename` 和 `package.searchpath` 共用 VFS 授权，但保留 LuaJIT 的参数和失败返回形式，不返回 File 专用错误表。没有原生库授权名单，`package.loadlib` 和磁盘 C 模块搜索器明确拒绝；自动系统临时文件入口 `io.tmpfile/os.tmpname` 也明确拒绝。

宿主 stdio、`io.popen/os.execute`、FFI、`load/loadstring` 与无文件名的 stdin 代码加载仍按实际 LuaJIT 能力暴露。这些能力不构成完整不可信脚本沙箱，字符串或流不会因此被标为已认证包代码。认证加密的目标是提高直接解包与简单替换的门槛，不提供反调试、签名发行者证明或防止持密钥者重打包的保证。

Shader 示例继续使用内建 `Engine.hlsl` 与现有渲染接口，见 [Shader 指南](shader.md)。File 与包的 CPU/互操作结果不能代替 Resource、Graphic、Audio 的组合测试或 GPU、音频设备验收。
