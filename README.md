


![Selen2D Banner](src/res/image/Selen2D_banner_2048x1024.png)

<p align="center">
  <strong>Selen2D, A lightweight, LuaJIT-driven game engine for 2D games.</strong><br />
  <strong>Selen2D, 轻量级 LuaJIT 驱动的 2D 游戏引擎。</strong><br />
  <strong>Selen2D, LuaJIT を中心とした軽量な 2D ゲームエンジン。</strong>
</p>
<p align="center">
  <img src="src/res/image/selen2d-logo.png" alt="Selen2D logo" width="104" />
</p>
<p align="center">
  <a href="https://github.com/phsonh/Selen2D/releases/latest">Latest release</a> ·
  <a href="doc/guide/README.md">Guides</a> ·
  <a href="doc/api/">Lua API</a> ·
  <a href="LICENSE">MIT License</a>
</p>

<p align="center"><a href="#english">English</a> · <a href="#简体中文">简体中文</a> · <a href="#日本語">日本語</a></p>

> **Current stable release: v2.1.0 · Windows x64 / Direct3D 12**  
> **Two ways to use Selen2D:** download the ready-to-run Release to write Lua immediately, or build a project-specific engine from source while keeping your game in a separate directory.  
> **v2.2.x development:** a PyQt6 project/packing GUI, automatic JSON-to-EXE configuration, and console visibility controls are implemented and being validated on `develop`. They are **not features of v2.1.0**.

### Current develop quick start / 当前开发版 / 開発版

The following workflows describe this `develop` checkout and its **unreleased
2.2.x development package**, not the contents of the published v2.1.0 ZIP.
本页后续流程对应当前 `develop` 与**尚未发布的 2.2.x 开发包**；正式版仍为 v2.1.0，其历史包与随包文档不变。
以下は現在の `develop` と**未公開の 2.2.x 開発パッケージ**の手順です。公開済み v2.1.0 の内容は変更されません。

```powershell
python -m venv build/tools-venv
./build/tools-venv/Scripts/python.exe -m pip install -r tools/pack/requirements-gui.txt
./build/tools-venv/Scripts/python.exe -B tools/pack/main.py
# GUI: edit launch/launch.json, Validate, Save, then build.
./tools/build.ps1 -Configuration Release
```

Game resources remain in a separate directory. The GUI edits **one fixed private
file, `launch/launch.json`**, initialized from the public default on first use.
Its configuration actions are **Save** and **Validate**; it has no profile
selection, New/Open/Save As or activation step. An ordinary `build.ps1` run uses
this private file when present, otherwise `src/res/development/launch.json`.
The script resolves that choice again on each invocation. Explicit CLI/CI
configuration remains available with `-LaunchConfig path/to/config.json`.
Without the GUI, install only `tools/pack/requirements-launch.txt`; compiling
the configuration does not require Qt, PyNaCl or Zstd.

游戏素材留在独立目录。GUI 只编辑被忽略的 `launch/launch.json`，首次使用从公开默认配置初始化，只提供“保存”和“验证”，没有多配置切换或激活步骤。普通构建优先使用该私有文件，不存在时使用公开默认 JSON。JSON 变更自动触发生成和编译，验证失败中止构建。运行包不读取 JSON，也不需要 Python。

ゲーム素材は外部ディレクトリのまま使用できます。GUI は初回に公開設定から作成した `launch/launch.json` だけを編集し、保存と検証を行います。通常ビルドはこの非公開設定を優先し、存在しなければ公開設定を使います。実行時に JSON や Python は不要です。

The standard development package uses this secret-free public configuration:
标准开发包／標準開発パッケージの公開設定：

```json
{
  "entry": "main.lua",
  "mounts": [],
  "directories": [
    { "base": "deployment", "path": ".", "target": "." }
  ]
}
```

Here `.` is a configuration-only marker for the EXE directory root. It does not
relax Lua path validation: VFS paths still use safe relative names, and cannot
escape through absolute paths, `..`, symbolic links or junctions.
这里的 `.` 仅是配置中的 EXE 根目录标记，不是允许 Lua 使用 `.`、`..` 或绝对路径；安全子路径仍统一经过 VFS。
`.` は設定専用の EXE ルート指定です。Lua の通常パスは引き続き VFS で検証されます。

Standard packages use an **isolated public build**, even when a private project exists:

```powershell
./tools/build.ps1 -Configuration Release -PublicRelease -StageRelease
```

This uses `build/public-release`, forces the secret-free public example, and
rejects private configuration. Ordinary/custom `stage_release` targets fail.
标准包继续直接解压运行，允许修改普通文件示例 Lua/HLSL；不会包含 JSON、工具、私有密钥或额外 DLL。
开发版 Windows 控制台默认隐藏，可用 `Debug.SetConsoleVisible(true/false)` 和
`Debug.IsConsoleVisible()` 控制；日志仍写入 EXE 旁的 `engine.log`。

See [GUI workflow / 图形工具](doc/guide/tools-gui.md),
[JSON and deployment / 自动构建与部署](doc/guide/file-launch.md), and
[console and diagnostics / 控制台与诊断](doc/guide/debug-console.md).

---

<a id="english"></a>
## English

### Overview

Selen2D is a small C++20 game engine built around **LuaJIT**, **SDL3 / SDL_GPU**, and a Lua-controlled game loop. It focuses on 2D games—including action games and bullet-hell/STG projects—while offering selected Canvas3D and shader capabilities. The engine provides native systems, not a prescribed scene framework: your Lua code decides how to organize gameplay.

**Highlights**

- **Lua-first runtime:** explicit frame scheduling, object lifecycle via `Object.Commit()`, deterministic-friendly random generators, and precise time utilities.
- **Graphics:** 2D Canvas and Canvas3D, rendering to offscreen targets, dynamic meshes, custom HLSL shaders via `Engine.hlsl`, blend modes, fog, PNG readback, and text layout/rendering.
- **Resources:** asynchronous loaders with atomic publication, image/font/shader/audio resources, and explicit audio-player objects.
- **Storage:** authorized virtual file system (VFS), writable directories, SLPK v1 packages, and authenticated encrypted SLPK v2 packages.
- **Developer experience:** Lua editor definitions in [`doc/api/`](doc/api/), practical guides, and an interactive **Orbit Garden** example.

The published binaries are validated for **Windows x64 with Direct3D 12**. Source code for other platform paths does not imply equivalent build or runtime support. Selen2D is an engine, not a full editor, scene system, or complete 3D framework.

### Choose your workflow

| | **A. Release / Lua developer** | **B. Source build / game-project developer** |
|---|---|---|
| Ideal for | Learning, prototyping, writing Lua and HLSL | Shipping your own project with a custom entry, VFS permissions or encrypted resources |
| Download | [Windows x64 Release](https://github.com/phsonh/Selen2D/releases/latest) | [Clone the engine repository](https://github.com/phsonh/Selen2D) |
| Build C++? | **No** | **Yes**, with `tools/build.ps1` |
| Python or packaging tools? | **No** | For preparing packages and launch configuration |
| Encrypted resource package? | Not required; default example uses loose files | Optional SLPK v2, configured with a project-specific secret |
| Where does game code live? | In an extracted copy of the Release, under authorized paths | In a **separate game-project directory**, not in the engine repository |

#### A · Start immediately with the standard package

The layout below describes the current development package. For the published
v2.1.0 ZIP, use its included guide; 2.2.x has not been released yet.

1. Obtain a verified standard ZIP, or use [Releases](https://github.com/phsonh/Selen2D/releases) for the current stable version.
2. Extract the ZIP to a writable directory and run `Selen2D.exe`.
3. Try the **Orbit Garden** sample: **Enter** to start, **WASD / arrow keys** to move, **P / Space** to pause, and **Esc** to return to the menu or quit.
4. Edit `example/main.lua` or an example HLSL file, save it, then restart the executable. There is no automatic hot reload.

```text
Selen2D.exe
main.lua
example/
  main.lua
  effect.hlsl
  palette.hlsl
  assets/
doc/
  api/
  guide/
```

The standard development package is **ready to run and unencrypted**. The EXE
starts the root `main.lua`, whose only job is to run the example through the
existing File API:

```lua
local ok, err = File.RunLuaFile("example/main.lua")
if not ok then
    Debug.Error(err.message)
end
```

Orbit Garden's actual logic, HLSL and assets remain under `example/`. You can
change the root wrapper to start your own safe relative Lua path without
recompiling. The project root is always the EXE directory, independent of the
working directory or script location. The default configuration authorizes
that root for VFS access, so `example/` and `output/` are not special allowlists.
Create folders as needed and use safe relative paths; absolute paths, `..` and
link-based escapes remain rejected. No external launch configuration, Python,
PyQt6, SDK or additional DLL is required in this runtime package.

The editor declarations under `doc/api/` are for autocomplete and type hints;
**do not execute them as game scripts**.

> The official Release is a general-purpose **engine example distribution**, not an encrypted build of any particular game.

#### B · Develop a separate game project from the engine source

**Keep this repository engine-only.** Do not move your game's Lua, assets, save data, or private keys into `src/`, `example/`, or the engine's Git history. A practical workspace looks like this:

```text
Workspace/
├── Selen2D/                    # engine source repository
│   ├── src/
│   ├── tools/
│   ├── launch/launch.json      # ignored private build configuration
│   └── ...
└── MyGame/                     # independent game project
    ├── content/                # files to put inside the package
    │   ├── main.lua
    │   ├── effect.hlsl
    │   └── assets/
    └── dist/                   # final, separately assembled game
```

**Current develop workflow (not part of the published v2.1.0):**

1. Clone Selen2D and create your independent game folder.
2. Start the **PyQt6 configuration GUI** (`python tools/pack/main.py`; install the GUI requirements first). It initializes `launch/launch.json` from the public default if missing. Edit the Lua entry, `mounts`, `parts`, directory mappings and optional secret, then **Validate** and **Save** this fixed configuration. There is no project-profile activation or Save As step.
3. Run `./tools/build.ps1 -Configuration Release`. The build system validates the selected JSON, generates an **internal build-only header**, and embeds the values in `Selen2D.exe`. JSON edits trigger regeneration; invalid configuration stops the build.
4. Use the **PyQt6 package GUI** with the **same configuration and secret** to pack and verify your game folder as SLPK v1 (plain) or v2 (authenticated/encrypted).
5. Place the game-specific executable and the referenced package files together in `MyGame/dist/`. Keep the project licenses/notices when redistributing.

**The game never reads `launch.json` or `launch.h` at runtime.** The configured entry point, mounts, secret and VFS permissions are fixed at compile time. A secret embedded in a client executable is not DRM or a guarantee against reverse engineering.

##### A project configuration example

This JSON is for an **independent game project**, not for the standard Selen2D Release:

```json
{
  "secret": "REPLACE_WITH_YOUR_OWN_PRIVATE_SECRET",
  "mounts": [
    {
      "source": "data.bin",
      "target": "game",
      "parts": 0,
      "encrypted": true
    }
  ],
  "directories": [
    {
      "base": "user_data",
      "path": "MyStudio/MyGame",
      "target": "save"
    }
  ],
  "entry": "game/main.lua"
}
```

- `source` is a package filename **relative to the executable's directory**, not the source folder used during packing.
- `target` is the virtual prefix visible to Lua. `game/main.lua` resolves to `main.lua` inside the package mounted at `game`.
- `parts` is a **globally unique mount priority**; higher numbers override matching paths from lower-priority **packages**. It is not a compression or pack-format setting.
- `encrypted: true` requires the same `secret` for building the package and the executable. An encrypted launch requires the Lua entry code to originate from an authenticated encrypted package.
- `base: "user_data"` uses the operating system's user-data root. On Windows, `save/settings.json` maps beneath `%LOCALAPPDATA%\MyStudio\MyGame\settings.json`; `deployment` instead refers to a folder beneath the executable directory.
- Package contents are read-only. Writable paths must be authorized through `directories`. Physical directory files cannot silently override package files at the same virtual path.

##### Current develop source workflow (GUI and CI)

Run from the engine root; MyGame remains outside it. `tools/pack/main.py` is the
single Python entry. Its implementation modules live in `selen2d_pytools/` and
are not separate commands. The GUI automatically uses the installed project
tools-venv.

```powershell
python -m venv build/tools-venv
$py = "./build/tools-venv/Scripts/python.exe"
& $py -m pip install -r tools/pack/requirements-gui.txt
python tools/pack/main.py
# GUI: edit launch/launch.json, Validate and Save.
./tools/build.ps1 -Configuration Release
New-Item -ItemType Directory -Force ../MyGame/dist | Out-Null
& $py tools/pack/main.py pack --source ../MyGame/content --output ../MyGame/dist/data.bin --encrypt --config launch/launch.json
& $py tools/pack/main.py verify --input ../MyGame/dist/data.bin --config launch/launch.json
Copy-Item build/release/bin/Selen2D.exe ../MyGame/dist/Selen2D.exe
```

GUI builds and encrypted packing use the same saved `launch/launch.json`.
The pack command above requires the encrypted configuration shown earlier;
choose plain SLPK v1 without `--encrypt` when encryption is unnecessary.
The CLI-only `-LaunchConfig` override remains available for CI. Without the
GUI, use `requirements-launch.txt` for compilation and `requirements-tools.txt`
for packing. Changing the secret requires recompiling the EXE and repacking
every encrypted package. Standard packages always use the isolated
`-PublicRelease` build, never the private project configuration.

### Build the engine itself

On Windows x64, install Visual Studio C++ tools (**MSVC 14.44** is the currently documented toolset), Windows SDK/ATL, CMake, Ninja and the build prerequisites described by the repository. In the engine source directory:

```powershell
.\tools\build.ps1 -Configuration Debug
.\tools\build.ps1 -Configuration Release
```

For an **isolated public example package** (maintainers):

```powershell
./tools/build.ps1 -Configuration Release -PublicRelease -StageRelease
```

A normal build updates `build/<configuration>/bin` using the private-first
configuration rule. `-PublicRelease` uses `build/public-release`, forces the
public secret-free JSON, and rejects private overrides. `-StageRelease` then
creates a ZIP under ignored `releases/`; Debug and ordinary custom builds cannot
stage the standard package. This local build command does not publish a GitHub
Release. Assemble project-specific game distributions separately.

### Documentation and API

| Guide | Contents |
|---|---|
| [Getting started](doc/guide/getting-started.md) | Run and edit the published example |
| [Orbit Garden example](doc/guide/example.md) | Menu, loading, gameplay, pause and cleanup |
| [Runtime](doc/guide/runtime.md) · [Input / Window](doc/guide/input-window.md) | Frame scheduling, coordinates and input |
| [Resources](doc/guide/resource.md) · [Audio](doc/guide/audio.md) | Atomic loading and audio players |
| [Shaders](doc/guide/shader.md) · [Text](doc/guide/text.md) | `Engine.hlsl`, Canvas rendering and text layout |
| [Files / packages / launch](doc/guide/file-launch.md) | VFS, startup configuration and Python CLI |
| [Lua API declarations](doc/api/) | Lua editor completion/type definitions; **not runtime code** |

### Branches, releases and scope

- **`main`** is the accepted stable branch. Official releases are tagged from an approved commit on `main`; [v2.1.0](https://github.com/phsonh/Selen2D/releases/tag/v2.1.0) is the current stable release.
- **`develop`** holds ongoing engine development and intermediate revisions. A completed feature release can later be merged into `main`. Published tags are not rewritten.
- **Unreleased v2.2.x development:** PyQt6 configuration/package UI, JSON-driven automatic embedding, root `main.lua` startup and `Debug.SetConsoleVisible(bool)` / `Debug.IsConsoleVisible()` with a hidden-by-default Windows console. Validation is ongoing on `develop`; **these changes are not part of v2.1.0**.
- The repository holds **engine code and public engine examples only**. Real game projects—including their assets, configuration and distribution—live elsewhere.

### License and third-party notices

Selen2D project-owned code is licensed under the [MIT License](LICENSE). **Copyright (c) 2026 Lonely Owl.** Bundled dependencies retain their own licenses and attribution requirements; see [`tools/info/THIRD_PARTY_NOTICES.txt`](tools/info/THIRD_PARTY_NOTICES.txt) and the notices/licenses inside distributed archives. Redistributing a custom game executable still requires the relevant third-party notices. The resource encryption feature provides authenticated packaging, **not a tamper-proof sandbox or copy protection**.

<p align="right"><a href="#english">↑ Back to English</a> · <a href="#简体中文">简体中文</a> · <a href="#日本語">日本語</a></p>

---

<a id="简体中文"></a>
## 简体中文

### 关于 Selen2D

**Selen2D** 是基于 **C++20、LuaJIT、SDL3 / SDL_GPU** 的轻量级游戏引擎，以 2D 游戏、动作游戏和弹幕 STG 为主要应用场景，并提供 Canvas3D、HLSL Shader 等部分 3D／后处理能力。引擎负责提供底层模块，**游戏流程由 Lua 自己组织**，不强制使用内建场景系统或固定的游戏框架。

主要能力包括：

- **运行时与对象**：Lua 主导帧调度、显式 `Object.Commit()`、对象生命周期、时间工具及可保存状态的随机数生成器。
- **图形与文字**：Canvas / Canvas3D、离屏绘制、Mesh 更新、`Engine.hlsl`、混合模式、Fog、PNG 读回、文字绘制和排版。
- **资源与音频**：异步 Loader、整批原子发布、图片/字体/Shader/音频加载以及独立播放器对象。
- **文件与资源包**：带授权范围的 VFS、可写目录、SLPK v1 明文包和 SLPK v2 认证加密包。
- **开发辅助**：[`doc/api/`](doc/api/) 编辑器声明、中文指南和可运行的 **Orbit Garden** 完整示例。

**当前正式验证平台为 Windows x64／Direct3D 12。** 其他平台存在相关代码不代表已经达到同等可用性；Canvas3D 也不等于提供一整套通用 3D 游戏框架。

### 先选择适合你的路线

| | **A. 普通开发者：下载 Release** | **B. 项目开发者：从源码定制构建** |
|---|---|---|
| 适合 | 学习 API、直接写 Lua、修改示例、快速原型 | 独立游戏项目、自定义入口/授权目录、加密资源包 |
| 获取方式 | [下载 Windows x64 发行包](https://github.com/phsonh/Selen2D/releases/latest) | [克隆 Selen2D 源码](https://github.com/phsonh/Selen2D) |
| 是否编译 C++ | **不需要** | **需要** |
| 是否需要 Python 打包工具 | **不需要** | 需要配置或打包时使用 |
| 是否必须加密 | **不需要**，默认使用普通文件 | **可选**，可制作 SLPK v2 加密包 |
| 游戏放哪里 | 解压后的独立目录中 | **单独的游戏项目目录**，不放进引擎仓库 |

#### A · 普通开发者：解压标准包后立即开始

以下目录与启动方式对应当前开发包。2.2.x 尚未发布；下载已发布的 v2.1.0 时，以其随包指南为准。

1. 获取经过验证的标准 ZIP；当前稳定版可从 [Releases](https://github.com/phsonh/Selen2D/releases) 下载。
2. 解压到可写目录，双击 `Selen2D.exe`。
3. 体验 **Orbit Garden**：**Enter** 开始、**WASD／方向键**移动、**P／空格**暂停、**Esc** 返回菜单或退出。
4. 修改 `example/main.lua`、`example/effect.hlsl` 等文件，保存后重启 EXE，即可看到效果；目前没有自动热重载。

```text
Selen2D.exe
main.lua
example/
  main.lua
  effect.hlsl
  palette.hlsl
  assets/
doc/
  api/
  guide/
```

**标准开发包不加密，解压即可运行。** EXE 启动根目录 `main.lua`，该文件只通过现有 File 接口转入示例：

```lua
local ok, err = File.RunLuaFile("example/main.lua")
if not ok then
    Debug.Error(err.message)
end
```

Orbit Garden 的实际逻辑、Shader 和素材仍在 `example/`。可修改根 `main.lua`，改为执行自己的安全相对路径脚本，不用重新编译。项目根始终是 EXE 所在目录，不随工作目录或当前脚本改变。默认配置授权这个根目录，**不限于 `example/` 或 `output/`**；可以按需创建其他子目录，所有文件操作仍统一通过 VFS。绝对路径、`..`、符号链接及 junction 越界仍会被拒绝。运行包无需外部启动配置、Python、PyQt6、SDK 或额外 DLL。

`doc/api/` 只用于 Lua 编辑器自动补全和类型提示，其中的声明文件**不是可以执行的 Lua 实现**，不要 `dofile` 或 `require` 它们。

> 官方 Release 是**开箱即用的通用引擎与示例**，不是任何特定游戏的加密发行包。

#### B · 项目开发者：在独立文件夹制作自己的游戏

**本仓库只负责开发 Selen2D 引擎。** 请把自己的游戏脚本、素材、私有密钥和发行文件放在独立项目文件夹，而不是塞进 `Selen2D/src/` 或引擎 Git 仓库。

```text
Workspace/
├── Selen2D/                  # 引擎仓库
│   ├── src/
│   ├── tools/
│   ├── launch/launch.json    # 被忽略的私有构建配置
│   └── ...
└── MyGame/                   # 独立游戏项目
    ├── content/              # 待打包资源的根目录
    │   ├── main.lua
    │   ├── effect.hlsl
    │   └── assets/
    └── dist/                 # 最终游戏部署目录
```

**当前 develop 的 PyQt6 工作流（已发布的 2.1.0 不包含）：**

1. 克隆 Selen2D，并在仓库外创建独立游戏项目。
2. 启动 **PyQt6 图形化配置工具**。首次使用会从公开默认配置初始化 `launch/launch.json`。设置 Lua 入口、资源包 `mounts`、挂载优先级 `parts`、读写目录映射和可选 `secret`，然后“验证”和“保存”。GUI 只编辑这份固定配置，没有新建／打开／另存为／激活或多项目配置切换。
3. 直接执行 `./tools/build.ps1 -Configuration Release`；构建系统自动验证选中的 JSON，在构建目录生成内部头文件，再把配置编译进 `Selen2D.exe`。JSON 改动会重新生成和编译，验证失败会中止构建。
4. 使用同一套 **PyQt6 打包工具**，读取**同一份 JSON 和密钥**，把 `content/` 打包成明文 SLPK v1 或认证加密 SLPK v2，并进行校验。
5. 将定制构建的 EXE 和资源包放到 `MyGame/dist/`，就可以运行；对外发行时还应保留所需的项目及第三方许可说明。

最终游戏运行时**不读取外部 JSON 或 `launch.h`**；入口、挂载、密钥和授权范围已在构建时写入 EXE。把密钥编入客户端能提高直接改包的门槛，但不意味着无法逆向提取，不能把它当成完整 DRM。

##### 项目 JSON 示例

下面的配置供**独立项目自定义构建**使用，不属于标准 Release 的默认配置：

```json
{
  "secret": "REPLACE_WITH_YOUR_OWN_PRIVATE_SECRET",
  "mounts": [
    {
      "source": "data.bin",
      "target": "game",
      "parts": 0,
      "encrypted": true
    }
  ],
  "directories": [
    {
      "base": "user_data",
      "path": "MyStudio/MyGame",
      "target": "save"
    }
  ],
  "entry": "game/main.lua"
}
```

字段含义：

- **`source`**：资源包的物理文件名，相对于 **EXE 所在目录**，不是打包时的输入目录。
- **`target`**：Lua 使用的虚拟目录前缀。包内 `main.lua` 映射成 `game/main.lua`。
- **`parts`**：所有挂载记录中不可重复的优先级数值。不同包同路径文件由较大 `parts` 覆盖；它**不是**打包命令的压缩参数。
- **`encrypted` 和 `secret`**：加密包必须使用相同密钥制作和编译；使用加密包时，Lua 入口代码也必须来自通过认证的加密包。
- **`directories`**：配置可访问的实际目录。`base: "deployment"` 表示 EXE 旁的子目录；`base: "user_data"` 表示系统用户数据根下的子目录。
- **`path` 与 `target`**：`path` 是真实基础目录下的路径，`target` 是 Lua 的虚拟别名。上例中 Lua 写 `save/settings.json`，Windows 实际对应 `%LOCALAPPDATA%\MyStudio\MyGame\settings.json`。

资源包只读；需要写入存档时，请使用获准的真实目录映射，按需调用 `File.CreateDirectory("save")`。资源包和真实目录的同名虚拟文件不能互相静默覆盖。GUI 的配置保存在被忽略的 `launch/launch.json`，游戏素材仍放在独立项目目录；不要提交真实 `secret`。

##### 当前 develop 的 GUI／命令行流程

在引擎根目录执行，MyGame 仍在仓库外。`tools/pack/main.py` 是唯一 Python 入口，`selen2d_pytools/` 只存放内部实现模块，不是另一组脚本入口。GUI 自动使用已经安装依赖的项目 tools-venv。

```powershell
python -m venv build/tools-venv
$py = "./build/tools-venv/Scripts/python.exe"
& $py -m pip install -r tools/pack/requirements-gui.txt
python tools/pack/main.py
# GUI: 编辑 launch/launch.json，验证并保存。
./tools/build.ps1 -Configuration Release
New-Item -ItemType Directory -Force ../MyGame/dist | Out-Null
& $py tools/pack/main.py pack --source ../MyGame/content --output ../MyGame/dist/data.bin --encrypt --config launch/launch.json
& $py tools/pack/main.py verify --input ../MyGame/dist/data.bin --config launch/launch.json
Copy-Item build/release/bin/Selen2D.exe ../MyGame/dist/Selen2D.exe
```

GUI 构建与加密打包使用同一份已保存的 `launch/launch.json`。上面的打包命令对应前文加密配置；不需要加密时使用明文 SLPK v1，不传 `--encrypt`。命令行 `-LaunchConfig` 仍可用于 CI 的显式配置。无 GUI 时，普通编译只需 `requirements-launch.txt`，打包需 `requirements-tools.txt`。密钥改变必须重新编译 EXE，并重打所有加密包。标准发行固定使用独立 `-PublicRelease` 构建，不采用私有配置。

### 构建引擎本身

Windows x64 下需要 Visual Studio C++ 工具（目前文档使用 **MSVC 14.44**）、Windows SDK／ATL、CMake、Ninja 及仓库注明的构建依赖：

```powershell
.\tools\build.ps1 -Configuration Debug
.\tools\build.ps1 -Configuration Release
```

维护者使用下面的独立公开构建，生成标准引擎示例 ZIP：

```powershell
./tools/build.ps1 -Configuration Release -PublicRelease -StageRelease
```

普通构建按“私有配置优先、公开配置回退”的规则更新 `build/<configuration>/bin`。`-PublicRelease` 使用独立的 `build/public-release`，强制选择无密钥的公开 JSON，并拒绝私有覆盖；加上 `-StageRelease` 才在被忽略的 `releases/` 生成 ZIP。Debug 和普通定制构建不能生成标准包，此命令也不会发布 GitHub Release。**独立游戏请另外部署自己的 EXE 和资源包。**

### 文档与 API

| 文档 | 内容 |
|---|---|
| [入门与修改示例](doc/guide/getting-started.md) | 解压、运行、修改 Lua |
| [Orbit Garden 完整示例](doc/guide/example.md) | 菜单、加载、运行、暂停、清理 |
| [Runtime](doc/guide/runtime.md) · [Input / Window](doc/guide/input-window.md) | 帧调度、窗口坐标、输入 |
| [Resource](doc/guide/resource.md) · [Audio](doc/guide/audio.md) | 资源管理、播放器 |
| [Shader](doc/guide/shader.md) · [Text](doc/guide/text.md) | `Engine.hlsl`、渲染和文字 |
| [File / 资源包 / 启动配置](doc/guide/file-launch.md) | VFS、SLPK、Python CLI |
| [Lua API 声明](doc/api/) | 编辑器补全和类型定义，**不可执行** |

### 版本、开发分支与边界

- **`main`** 只保留经过验收的稳定发布版本；当前正式版是 [v2.1.0](https://github.com/phsonh/Selen2D/releases/tag/v2.1.0)。
- **`develop`** 用于引擎后续持续开发、中间修订与回归测试。新版本完成验收后才合并到 `main`；已发布标签不改写。
- **尚未发布的 2.2.x 开发**：PyQt6 配置与资源包 GUI、JSON 自动编译、根 `main.lua` 启动壳、`Debug.SetConsoleVisible(bool)` / `Debug.IsConsoleVisible()` 及 Windows 控制台默认隐藏。目前在 `develop` 验证，**这些改动不属于 v2.1.0**。
- 本仓库仅存放引擎代码与公开示例；**任何独立游戏项目都应放在仓库之外**。

### 许可证与第三方组件

Selen2D 自有代码采用 [MIT 许可证](LICENSE)，**Copyright (c) 2026 Lonely Owl**。第三方依赖保留各自许可证及版权说明，详见 [`tools/info/THIRD_PARTY_NOTICES.txt`](tools/info/THIRD_PARTY_NOTICES.txt) 和发行包附带的许可文件。即使游戏使用静态链接的 EXE，转发时仍需遵守相关许可与通知要求。资源包加密提供认证封装能力，**不等于不可逆向的保护或完整脚本沙箱**。

<p align="right"><a href="#english">English</a> · <a href="#简体中文">↑ 返回简体中文</a> · <a href="#日本語">日本語</a></p>

---

<a id="日本語"></a>
## 日本語

### Selen2D について

**Selen2D** は **C++20、LuaJIT、SDL3 / SDL_GPU** を使用した軽量なゲームエンジンです。2D ゲーム、アクションゲーム、弾幕シューティング（STG）を主な対象とし、Canvas3D や HLSL シェーダーによる一部の 3D 表現・ポストプロセスにも対応しています。決まったシーン管理方式を強制せず、**ゲームの進行とフレーム処理は Lua 側で設計できます**。

主な機能：

- **ランタイム・オブジェクト**：Lua 主導のフレーム処理、明示的な `Object.Commit()`、オブジェクトのライフサイクル、時間処理、状態を保存できる乱数生成器。
- **グラフィックス・テキスト**：Canvas / Canvas3D、オフスクリーン描画、メッシュ更新、`Engine.hlsl`、ブレンド、Fog、PNG 読み戻し、文字組版・描画。
- **リソース・オーディオ**：非同期 Loader、バッチ単位のアトミックな公開、画像・フォント・シェーダー・音声、独立したオーディオプレイヤー。
- **ファイル・パッケージ**：アクセス範囲を明示する VFS、書き込み可能なディレクトリ、SLPK v1 と認証付き暗号化 SLPK v2。
- **開発支援**：[Lua API 定義](doc/api/)、ガイド、メニューから終了処理まで確認できる **Orbit Garden** サンプル。

**現在正式に検証されている環境は Windows x64 / Direct3D 12 です。** 他の OS 向けのコードがあっても、同等の動作確認が済んでいるとは限りません。Canvas3D は汎用 3D ゲームエンジン一式を意味するものではありません。

### 2 つの使い方

| | **A. 一般の開発者：Release を利用** | **B. ゲームプロジェクト開発者：ソースから専用ビルド** |
|---|---|---|
| 用途 | Lua の学習、サンプルの改造、試作 | 独立したゲーム、独自エントリーポイント、VFS 設定、暗号化リソース |
| 入手 | [Windows x64 Release](https://github.com/phsonh/Selen2D/releases/latest) | [エンジンのソース](https://github.com/phsonh/Selen2D) |
| C++ のビルド | **不要** | **必要** |
| Python ツール | **不要** | 設定生成・パッケージ作成に使用 |
| 暗号化 | デフォルトでは不要 | 必要に応じて SLPK v2 を選択 |
| ゲームの配置 | Release の展開先の許可されたフォルダー | **エンジンとは別のゲームプロジェクトディレクトリ** |

#### A · 標準パッケージを展開してすぐに始める

以下は現在の開発パッケージの構成です。2.2.x は未公開です。公開済み v2.1.0 を使用する場合は、その ZIP に同梱されたガイドを参照してください。

1. 検証済みの標準 ZIP を用意します。現在の安定版は [Releases](https://github.com/phsonh/Selen2D/releases) から入手できます。
2. 書き込み可能な場所に展開し、`Selen2D.exe` を実行します。
3. **Orbit Garden** を試します。**Enter** で開始、**WASD／矢印キー**で移動、**P／Space** で一時停止、**Esc** でメニューに戻るか終了します。
4. `example/main.lua` や `example/effect.hlsl` などを編集し、保存後に EXE を再起動します。自動ホットリロードはありません。

```text
Selen2D.exe
main.lua
example/
  main.lua
  effect.hlsl
  palette.hlsl
  assets/
doc/
  api/
  guide/
```

**標準開発パッケージは暗号化されておらず、展開して実行できます。** EXE はルートの `main.lua` を起動します。このファイルは既存の File API を使ってサンプルに処理を渡します。

```lua
local ok, err = File.RunLuaFile("example/main.lua")
if not ok then
    Debug.Error(err.message)
end
```

Orbit Garden の実際のロジック、HLSL、素材は `example/` に残ります。ルートの `main.lua` を編集すれば、再コンパイルせずに別の安全な相対パスのスクリプトを起動できます。プロジェクトのルートは常に EXE のディレクトリで、作業ディレクトリやスクリプトの位置には依存しません。デフォルト設定はこのルートを VFS に公開するため、**`example/` と `output/` だけに制限されません**。必要なサブディレクトリを作成して利用できますが、絶対パス、`..`、シンボリックリンクや junction による範囲外アクセスは禁止されます。実行パッケージに外部起動設定、Python、PyQt6、SDK、追加 DLL は不要です。

`doc/api/` はエディタ補完・型情報のための定義です。**実行用 Lua スクリプトとして読み込まないでください。**

> 公式 Release は、学習や試作用の**汎用エンジンとサンプル**です。特定のゲーム向けに暗号化・設定された配布物ではありません。

#### B · エンジンとは別の場所でゲームを制作する

**このリポジトリは Selen2D エンジンの開発専用です。** ゲームの Lua・素材・秘密鍵・配布ファイルをエンジンの `src/` や Git 履歴へ混ぜないでください。

```text
Workspace/
├── Selen2D/                   # エンジンのリポジトリ
│   ├── src/
│   ├── tools/
│   ├── launch/launch.json     # Git の対象外の非公開ビルド設定
│   └── ...
└── MyGame/                    # 独立したゲームプロジェクト
    ├── content/
    │   ├── main.lua
    │   ├── effect.hlsl
    │   └── assets/
    └── dist/
```

**現在の develop の PyQt6 手順（公開済み v2.1.0 には含まれません）：**

1. Selen2D をクローンし、別ディレクトリにゲームプロジェクトを作ります。
2. **PyQt6 設定 GUI** は初回に公開設定から `launch/launch.json` を作成します。この固定ファイルに Lua の起動先、`mounts`、`parts`、ディレクトリ、必要なら `secret` を設定して、検証・保存します。新規作成、開く、別名保存、有効化、複数プロファイルの切り替えはありません。
3. `./tools/build.ps1 -Configuration Release` を実行します。JSON を検証し、ビルドディレクトリ内のヘッダーを生成して、設定を `Selen2D.exe` に組み込みます。JSON を変更すると再生成・再コンパイルされ、不正な設定ではビルドが失敗します。
4. **PyQt6 パッケージ GUI** で**同じ JSON と秘密情報**を使用して `content/` を SLPK v1（平文）または SLPK v2（認証付き暗号化）として作成・検証します。
5. 専用ビルドの EXE とパッケージを `MyGame/dist/` に配置します。配布時には必要なライセンス・著作権表示も同梱してください。

ゲーム実行時に外部 `launch.json`／`launch.h` を読み込むことはありません。起動先、マウント、秘密情報、VFS の許可範囲は**ビルド時に EXE に固定**されます。ただしクライアント EXE に埋め込んだ秘密情報を完全に隠せるわけではなく、DRM の代替にはなりません。

##### ゲーム専用 JSON の例

以下は**独立したゲーム用のカスタムビルド**例で、公式 Release の設定ではありません。

```json
{
  "secret": "REPLACE_WITH_YOUR_OWN_PRIVATE_SECRET",
  "mounts": [
    {
      "source": "data.bin",
      "target": "game",
      "parts": 0,
      "encrypted": true
    }
  ],
  "directories": [
    {
      "base": "user_data",
      "path": "MyStudio/MyGame",
      "target": "save"
    }
  ],
  "entry": "game/main.lua"
}
```

設定の意味：

- **`source`**：EXE と同じディレクトリを基準とするパッケージの相対パス。パッキング時の入力元ではありません。
- **`target`**：Lua から見える仮想パスの接頭辞。パッケージ内の `main.lua` は `game/main.lua` になります。
- **`parts`**：マウント全体で重複できない優先順位。複数パッケージの同名パスは大きい値が優先されます。圧縮レベルではありません。
- **`encrypted`／`secret`**：暗号化する場合、パッケージ生成時と EXE のビルド時で同じ秘密情報を使います。暗号化が設定されているとき、Lua の起動コードも認証済みの暗号化パッケージ内に必要です。
- **`directories`**：読み書きを許可する実ディレクトリの一覧。`base: "deployment"` は EXE 側、`base: "user_data"` は OS のユーザーデータ領域です。
- **`path`／`target`**：実ディレクトリ内の相対パスと Lua 側の仮想名です。この例では `save/settings.json` が Windows 上で `%LOCALAPPDATA%\MyStudio\MyGame\settings.json` に対応します。

パッケージ内は読み取り専用です。セーブデータは許可された実ディレクトリに書き込み、必要に応じて `File.CreateDirectory("save")` などで作成します。実ディレクトリとパッケージで同じ仮想ファイル名を使っても、自動的に上書きされることはありません。GUI の設定は Git の対象外の `launch/launch.json` に保存し、ゲーム素材は外部に置きます。実際の秘密情報を公開 Git にコミットしないでください。

##### 現在の develop の GUI／CLI 手順

エンジンのルートで実行します。MyGame はリポジトリの外に置きます。Python の唯一の入口は `tools/pack/main.py` です。`selen2d_pytools/` は内部実装で、別のコマンド入口ではありません。GUI は依存関係をインストールした専用 tools-venv を自動選択します。

```powershell
python -m venv build/tools-venv
$py = "./build/tools-venv/Scripts/python.exe"
& $py -m pip install -r tools/pack/requirements-gui.txt
python tools/pack/main.py
# GUI: launch/launch.json を編集、検証、保存。
./tools/build.ps1 -Configuration Release
New-Item -ItemType Directory -Force ../MyGame/dist | Out-Null
& $py tools/pack/main.py pack --source ../MyGame/content --output ../MyGame/dist/data.bin --encrypt --config launch/launch.json
& $py tools/pack/main.py verify --input ../MyGame/dist/data.bin --config launch/launch.json
Copy-Item build/release/bin/Selen2D.exe ../MyGame/dist/Selen2D.exe
```

GUI のビルドと暗号化パッキングは、保存済みの同じ `launch/launch.json` を使用します。上記コマンドは前述の暗号化設定に対応します。暗号化が不要なら `--encrypt` を外して SLPK v1 を使います。CLI の `-LaunchConfig` は CI 用の明示指定として利用できます。GUI なしのコンパイルには `requirements-launch.txt`、パッキングには `requirements-tools.txt` を使用します。秘密情報を変更したら EXE と全暗号化パッケージを作り直します。標準配布には独立した `-PublicRelease` ビルドを使い、非公開設定を混入させません。

### エンジン本体のビルド

Windows x64 では Visual Studio C++ ツール（現行ドキュメントの対象は **MSVC 14.44**）、Windows SDK／ATL、CMake、Ninja などが必要です。

```powershell
.\tools\build.ps1 -Configuration Debug
.\tools\build.ps1 -Configuration Release
```

管理者が独立した公開ビルドで標準サンプル ZIP を作成する場合は、次のコマンドを使用します。

```powershell
./tools/build.ps1 -Configuration Release -PublicRelease -StageRelease
```

通常ビルドは非公開設定を優先し、存在しなければ公開設定を使って `build/<configuration>/bin` を更新します。`-PublicRelease` は独立した `build/public-release` と秘密情報のない公開 JSON を使用し、非公開設定の上書きを拒否します。`-StageRelease` を付けると Git 対象外の `releases/` に ZIP を生成します。Debug や通常の専用ビルドでは標準 ZIP を作れません。このコマンドによる GitHub Release の公開は行いません。**ゲーム専用の配布物は別途管理してください。**

### ガイドと API

| ドキュメント | 内容 |
|---|---|
| [はじめに](doc/guide/getting-started.md) | 配布物の実行と Lua の編集 |
| [Orbit Garden](doc/guide/example.md) | メニュー、非同期ロード、ゲーム進行、停止とクリーンアップ |
| [Runtime](doc/guide/runtime.md) · [Input / Window](doc/guide/input-window.md) | フレーム処理、座標系、入力 |
| [Resource](doc/guide/resource.md) · [Audio](doc/guide/audio.md) | リソース管理、プレイヤー |
| [Shader](doc/guide/shader.md) · [Text](doc/guide/text.md) | `Engine.hlsl`、描画、文字組版 |
| [File / パッケージ / 起動設定](doc/guide/file-launch.md) | VFS、SLPK、Python CLI |
| [Lua API 定義](doc/api/) | エディタ補完・型情報。**実行用ではありません** |

### ブランチ、リリース、今後の予定

- **`main`** は検証済みの安定版ブランチです。現行の正式リリースは [v2.1.0](https://github.com/phsonh/Selen2D/releases/tag/v2.1.0) です。
- **`develop`** はエンジンの継続開発と途中の修正用です。機能を検証した後に `main` へ反映します。公開済みタグは変更しません。
- **未公開の v2.2.x 開発**：PyQt6 設定／パッケージ GUI、JSON の自動組み込み、ルート `main.lua` の起動ラッパー、`Debug.SetConsoleVisible(bool)`／`Debug.IsConsoleVisible()`、Windows コンソールのデフォルト非表示。現在 `develop` で検証中であり、**v2.1.0 には含まれません**。
- このリポジトリは**エンジンと公開サンプル専用**です。実ゲームのプロジェクトは別の場所に保存してください。

### ライセンスとサードパーティー

Selen2D 独自コードは [MIT License](LICENSE) で公開されています。**Copyright (c) 2026 Lonely Owl.** 同梱ライブラリには個別のライセンスと表示義務があります。[`tools/info/THIRD_PARTY_NOTICES.txt`](tools/info/THIRD_PARTY_NOTICES.txt) と配布 ZIP 内のライセンス文書を確認してください。静的リンクされた EXE でも必要な表示は引き継がれます。リソース暗号化は認証付きパッケージ機能であり、**完全なコピー防止や未信頼スクリプト向けサンドボックスではありません**。

<p align="right"><a href="#english">English</a> · <a href="#简体中文">简体中文</a> · <a href="#日本語">↑ 日本語の先頭へ</a></p>
