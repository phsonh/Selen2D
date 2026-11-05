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
<p align="center">
  <a href="#english">English</a> · <a href="#简体中文">简体中文</a> · <a href="#日本語">日本語</a>
</p>

---

<a id="english"></a>
## English

### Selen2D

Selen2D is a C++20 game engine with **LuaJIT** scripting and **SDL3 / SDL_GPU** rendering. It provides windowing, input, graphics, audio, resources, and file access through Lua. Game code controls the frame loop, object updates, and draw order.

The v2.2.0 release targets **Windows x64 / Direct3D 12**. The executable includes its shader compiler and other native runtime components; running a game requires no Python installation or separate third-party runtime DLLs.

[LÖVE](https://love2d.org/) provides a Lua game framework with conventional callbacks. [Defold](https://defold.com/) combines Lua scripting with an editor and content pipeline. Selen2D keeps its workflow close to Lua code, explicit rendering targets, and a small set of native systems.

### Hello World

Download the [Windows release](https://github.com/phsonh/Selen2D/releases/latest), extract it, and replace the root `main.lua` with this example. Keep `example/assets/NotoSans-Regular.ttf`, which is included in the ZIP.

```lua
Window.SetTitle("Selen2D | Hello World")
Window.SetSize(960, 540)
Resource.LoadFont("hello", "example/assets/NotoSans-Regular.ttf")
Graphic.SetTextAnchor("center")
local screen = Graphic.GetWindowCanvas()
Runtime.SetFrameFunction(function()
    screen:Clear(0xFF0C1424)
    screen:RenderText("hello", "Hello, Selen2D!", 0, 30, 54, 0xFF72DBC9, "mul+alpha", 0)
    screen:RenderText("hello", "LuaJIT  /  SDL3  /  SDL_GPU", 0, -40, 22, 0xFFB5C7DD, "mul+alpha", 1)
end)
```

<p align="center">
  <a href="doc/example/hello_world_test2.png"><img src="doc/example/hello_world_test2.png" width="640" alt="Selen2D Hello World running on Windows" /></a>
</p>
<p align="center"><sub>Actual Windows runtime capture · Click to view at full size</sub></p>

<details>
<summary>Source code screenshot</summary>

![The Hello World source in a Lua editor](doc/example/hello_world_test1.png)

</details>

### Features

- **Lua-controlled runtime** — Register frame and exit callbacks, choose when to advance objects, and use `Object.Commit()` to apply lifecycle changes.
- **2D rendering and Canvas3D** — Draw to the window or offscreen canvases, use meshes and blend modes, and compile custom HLSL shaders with `Engine.hlsl`.
- **Text and audio** — Load fonts, render and measure text, and control sound or music through independent audio players.
- **Resource loading** — Load images, shaders, fonts, and audio through the resource system, with asynchronous batch loading when needed.
- **Files and packages** — Access authorized virtual paths; read plain SLPK v1 packages or authenticated encrypted SLPK v2 packages.
- **Development tools** — Edit build-time startup settings, pack resources, and compile a project-specific executable with the PyQt6 tool. Lua API definitions are available in [`doc/api/`](doc/api/).

### Use the ready-made release

The standard release is the quickest way to start. Extract the ZIP and run `Selen2D.exe`:

```text
Selen2D/
├── Selen2D.exe
├── main.lua
├── example/
│   ├── main.lua
│   ├── effect.hlsl
│   ├── palette.hlsl
│   └── assets/
└── doc/
    ├── api/
    └── guide/
```

The root `main.lua` initially contains:

```lua
local ok, err = File.RunLuaFile("example/main.lua")
if not ok then
    Debug.Error(err.message)
end
```

This starts **Orbit Garden**, the included interactive example. Press **Enter** to begin, move with **WASD / arrow keys**, pause with **P / Space**, and use **Esc** to return to the menu or exit.

Edit `main.lua` to launch your own Lua files. You can create `scripts/`, `assets/`, `shaders/`, or other folders beside the executable. The default configuration permits access to safe relative paths inside that directory. Changes to Lua and HLSL files take effect after restarting the game.

There is no setup step for the standard release. Python, the project JSON, and the packaging GUI stay in the source checkout.

### Build an executable for your own project

Keep the engine checkout and game content in separate directories. One possible layout is:

```text
Workspace/
├── Selen2D/                # Engine source and build tools
└── MyGame/
    ├── content/
    │   └── main.lua
    └── dist/                # Executable and packages for the game
```

**1. Install and open Selen2D Tools.** From the engine repository, with Python 3.10+ and the Windows C++ build toolchain installed:

```powershell
python -m venv build/tools-venv
.\build\tools-venv\Scripts\python.exe -m pip install -r tools/pack/requirements-gui.txt
.\build\tools-venv\Scripts\python.exe -B tools/pack/main.py
```

The PyQt6 window edits the single private file `launch/launch.json`. It creates that file from the public default on first use. Configure the entry, package mounts, optional encryption secret, and directory mappings, then click **Save configuration**. The engine build reads the same JSON file.

For a project using an encrypted `data.slpk`, a typical configuration is:

```json
{
  "secret": "replace-with-your-own-secret",
  "mounts": [
    { "source": "data.slpk", "target": "game", "parts": 0, "encrypted": true }
  ],
  "directories": [
    { "base": "local_app_data", "path": "MyStudio/MyGame", "target": "save" }
  ],
  "entry": "game/main.lua"
}
```

Here `source` is relative to the final EXE directory; `target` is the path visible to Lua. The package's `main.lua` becomes `game/main.lua`. Files under `save/...` map to `%LOCALAPPDATA%\MyStudio\MyGame\...` on Windows. `parts` controls priority between packages: higher values override lower values at the same virtual path.

**2. Build the engine with those settings.** The build validates JSON, generates an internal C++ header, and embeds the startup configuration in `Selen2D.exe`.

```powershell
.\tools\build.ps1 -Configuration Release
```

The executable is written to `build/release/bin/Selen2D.exe`. You can also start the build from the GUI. `Debug` is available in place of `Release`.

**3. Pack your game files.** In Selen2D Tools, select `MyGame/content/` as the source and `MyGame/dist/data.slpk` as the output. Choose **SLPK v2 encrypted** to use the secret from `launch/launch.json`. The tool verifies the package after writing it. Plain SLPK v1 packages are also supported.

**4. Deploy.** Copy the built executable beside the package:

```text
MyGame/dist/
├── Selen2D.exe
└── data.slpk
```

The executable loads the compiled entry and mounts the named package at startup. Neither the JSON file nor Python is needed on the player's machine. Keep project secrets private; a secret embedded in an executable can still be extracted by someone analyzing that binary.

When `launch/launch.json` has not been created, source builds use `src/res/development/launch.json`: entry `main.lua` and the `root / . → .` directory mapping. The official release build always uses an isolated public configuration, regardless of private project settings.

### Source builds and documentation

Windows source builds require **MSVC (Visual Studio C++ tools)**, the **Windows SDK**, **CMake**, **Ninja**, and Python for build-time configuration. Normal source builds use:

```powershell
.\tools\build.ps1 -Configuration Debug
.\tools\build.ps1 -Configuration Release
```

Diagnostics use stdout (Info/Debug) and stderr (Warn/Error/Fatal). No console window or log file is created automatically. `Debug.GetLog()` returns a bounded snapshot of Lua logs and reported Lua errors; use the authorized File API to save it if desired.

Documentation: [Getting started](doc/guide/getting-started.md) · [Selen2D Tools](doc/guide/tools-gui.md) · [Shaders](doc/guide/shader.md) · [Resources and startup configuration](doc/guide/file-launch.md) · [Lua API](doc/api/).

**License:** Selen2D engine code is licensed under [MIT](LICENSE), Copyright © 2026 Lonely Owl. Retain the bundled third-party notices when redistributing the engine. PyQt6 is an optional developer-tool dependency with its own GPL/commercial licensing terms.

---

<a id="简体中文"></a>
## 简体中文

### Selen2D

Selen2D 是一款使用 **LuaJIT** 编写游戏逻辑、以 **C++20** 和 **SDL3 / SDL_GPU** 实现底层功能的轻量级游戏引擎。窗口、输入、图形、音频、资源和文件访问都有对应的 Lua 接口。每帧要更新什么、何时处理对象生命周期、按什么顺序绘制，都由 Lua 代码安排。

v2.2.0 提供经过验证的 **Windows x64 / Direct3D 12** 版本。Shader 编译器及所需原生运行组件集成在可执行文件中，运行游戏无需安装 Python，也无需额外部署第三方运行库 DLL。

[LÖVE](https://love2d.org/) 提供基于 Lua 回调的游戏框架；[Defold](https://defold.com/) 提供编辑器和资源构建流程。Selen2D 的使用方式更接近直接编写 Lua，通过明确的 API 控制对象、画布、资源和帧流程。

### Hello World

下载 [Windows 发行包](https://github.com/phsonh/Selen2D/releases/latest)，解压后用下面的代码替换根目录 `main.lua`。保留发行包自带的 `example/assets/NotoSans-Regular.ttf` 字体文件。

```lua
Window.SetTitle("Selen2D | Hello World")
Window.SetSize(960, 540)
Resource.LoadFont("hello", "example/assets/NotoSans-Regular.ttf")
Graphic.SetTextAnchor("center")
local screen = Graphic.GetWindowCanvas()
Runtime.SetFrameFunction(function()
    screen:Clear(0xFF0C1424)
    screen:RenderText("hello", "Hello, Selen2D!", 0, 30, 54, 0xFF72DBC9, "mul+alpha", 0)
    screen:RenderText("hello", "LuaJIT  /  SDL3  /  SDL_GPU", 0, -40, 22, 0xFFB5C7DD, "mul+alpha", 1)
end)
```

<p align="center">
  <a href="doc/example/hello_world_test2.png"><img src="doc/example/hello_world_test2.png" width="640" alt="Selen2D 在 Windows 上运行 Hello World 的实际画面" /></a>
</p>
<p align="center"><sub>Windows 实际运行截图 · 点击查看原图</sub></p>

<details>
<summary>查看代码截图</summary>

![编辑器中的 Hello World Lua 源码](doc/example/hello_world_test1.png)

</details>

### 主要功能

- **Lua 主导的运行流程**：注册帧回调和退出回调，通过 `Object.Commit()` 明确处理对象生命周期。
- **2D 绘制与 Canvas3D**：窗口和离屏画布、Mesh、Blend、Fog，以及通过 `Engine.hlsl` 使用自定义 HLSL Shader。
- **文字与音频**：字体加载、文字绘制和测量；独立播放器控制音效、音乐和声像。
- **资源管理**：图像、Shader、字体、音频资源及支持整批发布的异步加载器。
- **文件与资源包**：受授权范围约束的虚拟文件系统，SLPK v1 明文包和 SLPK v2 认证加密包。
- **开发工具**：PyQt6 图形化配置、资源打包与构建，配套 [`doc/api/`](doc/api/) Lua 编辑器声明。

### 路线一：下载 Release，直接编写 Lua

从 [Releases](https://github.com/phsonh/Selen2D/releases/latest) 下载 Windows ZIP，解压后运行 `Selen2D.exe`。

```text
Selen2D/
├── Selen2D.exe
├── main.lua
├── example/
│   ├── main.lua
│   ├── effect.hlsl
│   ├── palette.hlsl
│   └── assets/
└── doc/
    ├── api/
    └── guide/
```

根目录 `main.lua` 默认负责启动示例：

```lua
local ok, err = File.RunLuaFile("example/main.lua")
if not ok then
    Debug.Error(err.message)
end
```

首次运行会进入 **Orbit Garden** 示例。按 **Enter** 开始，使用 **WASD / 方向键**移动，按 **P / 空格**暂停，按 **Esc** 返回菜单或退出。

你可以直接修改根目录 `main.lua`，让它加载自己的 Lua 文件；也可以在 EXE 旁创建 `scripts/`、`assets/`、`shaders/` 等目录。默认配置允许访问部署目录中的合法相对路径。修改 Lua、HLSL 后重新启动程序即可生效。

普通发行包不需要手动配置 JSON、安装 Python 或编译引擎。

### 路线二：从源码构建独立游戏项目

引擎源码和游戏内容建议分别存放。例如：

```text
Workspace/
├── Selen2D/                # 引擎源码及构建工具
└── MyGame/
    ├── content/
    │   └── main.lua
    └── dist/                # 游戏部署目录
```

**1. 安装并打开 Selen2D Tools。** 在引擎仓库根目录执行以下命令，需要 Python 3.10+。编译 EXE 还需 Windows C++ 构建工具链。

```powershell
python -m venv build/tools-venv
.\build\tools-venv\Scripts\python.exe -m pip install -r tools/pack/requirements-gui.txt
.\build\tools-venv\Scripts\python.exe -B tools/pack/main.py
```

PyQt6 GUI 只维护一份私有启动配置：`launch/launch.json`。首次打开会从公开模板创建。设置入口、资源包挂载、加密密钥和目录映射后，点击**保存配置**。后续构建直接读取这份 JSON。

例如，需要加密资源包和用户存档目录时，可以使用：

```json
{
  "secret": "replace-with-your-own-secret",
  "mounts": [
    { "source": "data.slpk", "target": "game", "parts": 0, "encrypted": true }
  ],
  "directories": [
    { "base": "local_app_data", "path": "MyStudio/MyGame", "target": "save" }
  ],
  "entry": "game/main.lua"
}
```

`source` 是相对于最终 EXE 目录的包文件位置，`target` 是 Lua 看到的虚拟目录。包内 `main.lua` 对应 `game/main.lua`。Lua 的 `save/...` 路径在 Windows 上指向 `%LOCALAPPDATA%\MyStudio\MyGame\...`。多个包使用不同的 `parts`：值越大，同虚拟路径的覆盖优先级越高。

**2. 构建自定义引擎。** 构建过程会校验 JSON，生成内部 C++ 头文件，并把启动信息编译进 EXE。

```powershell
.\tools\build.ps1 -Configuration Release
```

输出位于 `build/release/bin/Selen2D.exe`。也可以在 GUI 中发起构建。需要调试版时改用 `-Configuration Debug`。

**3. 打包资源。** 在 GUI 的资源包页面，选择独立游戏项目的 `MyGame/content/` 文件夹，输出到 `MyGame/dist/data.slpk`。选择 **SLPK v2 加密**即可使用同一份 JSON 中的密钥。工具会在生成资源包后验证内容。也可以选择不加密的 SLPK v1。

**4. 整理发布文件。** 将构建好的 EXE 复制到游戏部署目录：

```text
MyGame/dist/
├── Selen2D.exe
└── data.slpk
```

引擎启动时使用已经编译进去的入口和挂载配置。玩家无需获取项目 JSON、Python 或 PyQt6。密钥请保存在私有配置中；将密钥编入 EXE 并不能阻止有能力分析二进制文件的人提取它。

如果尚未创建私有 `launch/launch.json`，源码构建会使用公开的 `src/res/development/launch.json`，其入口为 `main.lua`，目录映射为 `root / . → .`。正式通用发行包始终使用独立的公开配置构建，不受私有项目 JSON 影响。

### 源码构建与文档

Windows 源码构建需要 **Visual Studio C++ 工具链（MSVC）**、**Windows SDK**、**CMake**、**Ninja** 以及用于编译期配置的 Python。常规构建命令：

```powershell
.\tools\build.ps1 -Configuration Debug
.\tools\build.ps1 -Configuration Release
```

诊断日志输出到 stdout（Info/Debug）及 stderr（Warn/Error/Fatal），不自动创建控制台或日志文件。`Debug.GetLog()` 返回有界的 Lua 日志及 Lua 错误快照，需要保存时通过已授权的 File API 写入。

详细资料：[入门指南](doc/guide/getting-started.md) · [Selen2D Tools](doc/guide/tools-gui.md) · [Shader](doc/guide/shader.md) · [文件与启动配置](doc/guide/file-launch.md) · [Lua API](doc/api/)。

**许可证：** 引擎自有代码采用 [MIT License](LICENSE)，Copyright © 2026 Lonely Owl。重新分发时请保留第三方许可与声明。PyQt6 为可选的开发工具依赖，其 GPL／商业许可需要单独遵守。

---

<a id="日本語"></a>
## 日本語

### Selen2D

Selen2D は **LuaJIT** でゲームロジックを記述できる軽量なゲームエンジンです。基盤には **C++20** と **SDL3 / SDL_GPU** を採用しています。ウィンドウ、入力、描画、音声、リソース、ファイル操作を Lua API から利用でき、フレーム処理やオブジェクトの更新、描画順序をゲーム側で組み立てられます。

v2.2.0 の対応・検証済み環境は **Windows x64 / Direct3D 12** です。Shader コンパイラと必要なネイティブランタイムは EXE に組み込まれており、実行時に Python や追加のサードパーティ製ランタイム DLL は必要ありません。

[LÖVE](https://love2d.org/) は Lua のコールバックを中心としたフレームワークを提供し、[Defold](https://defold.com/) はエディタとアセット構築の環境を備えています。Selen2D は Lua コードからフレーム処理、描画先、リソースの扱いを明示的に指定するスタイルです。

### Hello World

[Windows リリース](https://github.com/phsonh/Selen2D/releases/latest)をダウンロードして展開し、ルートの `main.lua` を次のコードに置き換えてください。ZIP に含まれる `example/assets/NotoSans-Regular.ttf` はそのまま残します。

```lua
Window.SetTitle("Selen2D | Hello World")
Window.SetSize(960, 540)
Resource.LoadFont("hello", "example/assets/NotoSans-Regular.ttf")
Graphic.SetTextAnchor("center")
local screen = Graphic.GetWindowCanvas()
Runtime.SetFrameFunction(function()
    screen:Clear(0xFF0C1424)
    screen:RenderText("hello", "Hello, Selen2D!", 0, 30, 54, 0xFF72DBC9, "mul+alpha", 0)
    screen:RenderText("hello", "LuaJIT  /  SDL3  /  SDL_GPU", 0, -40, 22, 0xFFB5C7DD, "mul+alpha", 1)
end)
```

<p align="center">
  <a href="doc/example/hello_world_test2.png"><img src="doc/example/hello_world_test2.png" width="640" alt="Windows で実行した Selen2D Hello World の画面" /></a>
</p>
<p align="center"><sub>Windows での実際の実行画面 · クリックで拡大</sub></p>

<details>
<summary>ソースコードのスクリーンショット</summary>

![Lua エディタに表示された Hello World のコード](doc/example/hello_world_test1.png)

</details>

### 主な機能

- **Lua 主導のランタイム**：フレーム・終了コールバックを登録し、`Object.Commit()` でオブジェクトの状態変更を反映します。
- **2D 描画と Canvas3D**：ウィンドウ・オフスクリーン Canvas、Mesh、Blend、Fog、`Engine.hlsl` を使用した HLSL Shader。
- **文字と音声**：フォントの読み込み、文字描画と計測、独立したプレイヤーによる効果音・音楽の制御。
- **リソース管理**：画像、Shader、フォント、音声の読み込みと、バッチ単位で公開する非同期ローダー。
- **ファイルとパッケージ**：許可範囲を持つ仮想ファイルシステム、平文 SLPK v1 と認証付き暗号化 SLPK v2。
- **開発ツール**：PyQt6 による設定・パッケージ作成・ビルド、[`doc/api/`](doc/api/) にある Lua エディタ用定義。

### 方法 1：リリース版ですぐに Lua を書く

[Releases](https://github.com/phsonh/Selen2D/releases/latest) から Windows ZIP を取得し、展開した `Selen2D.exe` を起動します。

```text
Selen2D/
├── Selen2D.exe
├── main.lua
├── example/
│   ├── main.lua
│   ├── effect.hlsl
│   ├── palette.hlsl
│   └── assets/
└── doc/
    ├── api/
    └── guide/
```

ルートの `main.lua` は最初からサンプルを起動するようになっています。

```lua
local ok, err = File.RunLuaFile("example/main.lua")
if not ok then
    Debug.Error(err.message)
end
```

同梱の **Orbit Garden** が起動します。**Enter** で開始し、**WASD／方向キー**で移動、**P／Space** で一時停止、**Esc** でメニューへ戻るか終了します。

ルートの `main.lua` を編集すれば、自分の Lua スクリプトを実行できます。EXE と同じ場所に `scripts/`、`assets/`、`shaders/` などのフォルダーを作成して利用できます。標準設定では配置先ディレクトリ内の安全な相対パスが許可されています。Lua や HLSL を変更した場合は、ゲームを再起動してください。

通常のリリース版では JSON の設定、Python のインストール、エンジンのコンパイルは必要ありません。

### 方法 2：ソースからプロジェクト専用 EXE を作る

エンジンのソースとゲームのデータは別々のディレクトリに置くことを推奨します。

```text
Workspace/
├── Selen2D/                # エンジンのソースとツール
└── MyGame/
    ├── content/
    │   └── main.lua
    └── dist/                # ゲームの配布先
```

**1. Selen2D Tools をインストールして起動します。** エンジンのリポジトリ直下で実行します。Python 3.10 以降が必要です。EXE のビルドには Windows C++ ビルドツールも必要です。

```powershell
python -m venv build/tools-venv
.\build\tools-venv\Scripts\python.exe -m pip install -r tools/pack/requirements-gui.txt
.\build\tools-venv\Scripts\python.exe -B tools/pack/main.py
```

PyQt6 の画面で編集する設定ファイルは `launch/launch.json` の一つだけです。初回起動時に公開テンプレートから作成されます。エントリースクリプト、パッケージのマウント、暗号化用の secret、ディレクトリの対応を設定して保存します。ビルドは同じ JSON を使用します。

暗号化パッケージとユーザーデータを利用する場合の設定例：

```json
{
  "secret": "replace-with-your-own-secret",
  "mounts": [
    { "source": "data.slpk", "target": "game", "parts": 0, "encrypted": true }
  ],
  "directories": [
    { "base": "local_app_data", "path": "MyStudio/MyGame", "target": "save" }
  ],
  "entry": "game/main.lua"
}
```

`source` は完成した EXE のディレクトリから見たパッケージの相対パス、`target` は Lua から見える仮想パスです。パッケージ内の `main.lua` は `game/main.lua` になります。`save/...` は Windows では `%LOCALAPPDATA%\MyStudio\MyGame\...` に対応します。同じ仮想パスに複数のパッケージがある場合は、`parts` の大きいものが優先されます。

**2. 設定を組み込んでビルドします。** JSON の検証と内部 C++ ヘッダーの生成を経て、起動設定が EXE に組み込まれます。

```powershell
.\tools\build.ps1 -Configuration Release
```

完成した EXE は `build/release/bin/Selen2D.exe` にあります。GUI のビルド画面からも実行できます。デバッグビルドには `-Configuration Debug` を使用します。

**3. ゲームデータをパッケージ化します。** GUI で `MyGame/content/` を入力、`MyGame/dist/data.slpk` を出力に指定します。**SLPK v2 暗号化**を選ぶと、同じ JSON に保存された secret を使ってパッケージを作成し、内容を検証します。平文の SLPK v1 も利用できます。

**4. ファイルを配置します。** ビルドした EXE をパッケージの横にコピーします。

```text
MyGame/dist/
├── Selen2D.exe
└── data.slpk
```

起動時には EXE に組み込まれたエントリーとマウント設定が使われます。プレイヤー側に JSON、Python、PyQt6 は必要ありません。secret は非公開で管理してください。EXE に埋め込んだ secret は、バイナリ解析によって取り出される可能性があります。

非公開の `launch/launch.json` がまだ作成されていない場合、ソースビルドは公開設定 `src/res/development/launch.json` を使用します。エントリーは `main.lua`、ディレクトリは `root / . → .` です。公式リリースは常に独立した公開設定でビルドされ、個別プロジェクトの JSON は組み込まれません。

### ソースビルドとドキュメント

Windows でソースをビルドするには、**Visual Studio C++ ツール（MSVC）**、**Windows SDK**、**CMake**、**Ninja**、ビルド時の設定に使う Python が必要です。

```powershell
.\tools\build.ps1 -Configuration Debug
.\tools\build.ps1 -Configuration Release
```

診断ログは stdout（Info/Debug）と stderr（Warn/Error/Fatal）へ出力します。コンソールやログファイルを自動作成しません。`Debug.GetLog()` は容量制限付きの Lua ログと Lua エラーのスナップショットを返します。必要に応じて、許可された File API で保存してください。

資料：[入門ガイド](doc/guide/getting-started.md) · [Selen2D Tools](doc/guide/tools-gui.md) · [Shader](doc/guide/shader.md) · [ファイルと起動設定](doc/guide/file-launch.md) · [Lua API](doc/api/)。

**ライセンス：** エンジン独自のコードは [MIT License](LICENSE)、Copyright © 2026 Lonely Owl。再配布時には同梱の第三者ライセンスと通知を保持してください。任意の開発ツール PyQt6 には別途 GPL／商用ライセンスが適用されます。
