# 开始运行与修改 Lua

将发行 ZIP 完整解压到一个独立项目目录，再运行其中的 `Selen2D.exe`。这是自包含运行目录：EXE、同目录的 `dxcompiler.dll`、示例和文档一起发布，不需要另行安装编译器或运行库。包中不放入 Python 打包工具、启动头文件、构建元数据或预先生成的日志，许可证随文档提供。当前发行面向 Windows x64 / Direct3D 12，不代表其他平台已经完成验收。

```text
Selen2D.exe
dxcompiler.dll -- 与 EXE 同次构建、同一配置的运行时 Shader 编译器
main.lua      -- File.RunLuaFile("example/main.lua") 启动引导
example/
  main.lua
  effect.hlsl
  palette.hlsl
  assets/     -- 原创 PNG/WAV、Noto Sans、OFL.txt 与 SOURCE.md
doc/
  api/
    modules/  -- Audio.lua、Graphic.lua 等功能模块声明
    runtime/  -- Runtime.lua
  guide/
    LICENSE
    THIRD_PARTY_NOTICES.txt
    CHANGELOG.md
    licenses/
```

启动配置已编译在 EXE 内。标准配置来自源码 `src/res/development/launch.json`，入口是根目录 `main.lua`，没有资源包或 secret。目录声明 `root / . → .` 授权 EXE 所在部署根及其安全子目录。运行时不读取外部 JSON 或 `launch.h`，没有失败回退配置。普通用户不需要 Python 或编译步骤。

根目录 `main.lua` 仅引导示例；Orbit Garden 的实际逻辑和全部资源仍在 `example/`：

```lua
local ok, err = File.RunLuaFile("example/main.lua")
if not ok then
    Debug.Error(err.message)
end
```

正常读取失败按返回值报告；语法或运行错误按 `File.RunLuaFile` 契约传播。可以直接修改根入口，让它执行自己安排的 Lua 文件，不需要修改 JSON 或重新编译。

双击 EXE 即可运行。Enter 从菜单开始，WASD 或方向键移动金色光点，P 或空格暂停；游戏中 Esc 返回菜单、菜单中 Esc 退出。标题栏也显示当前操作方式。示例保留 `Window.GetTitle()` 返回的基础标题，再追加自己的说明，不读取构建元数据。完整状态流和 `--self-test` 用法见 [Orbit Garden 示例](example.md)。

Lua 使用相对 VFS 根的虚拟路径。默认部署根不随终端工作目录、快捷方式或当前脚本改变。示例仍以 `example/effect.hlsl` 等完整虚拟路径加载资源；你可以创建自己的 `assets/`、`scripts/` 等目录。`.` 只是配置中的特殊根标记，Lua 路径仍禁止 `.`、`..`、绝对路径和链接逃逸。

默认运行不需要资源包、SDK、tools 目录或 Python。缺少根 `main.lua` 或同目录的 `dxcompiler.dll` 会导致启动失败。子目录不会自动创建；写入前可调用 `File.CreateDirectory('save')`。`output/` 只是示例自测选择的输出位置，不是额外权限边界。标准 Lua 的文件名入口也经过 VFS，保留各自的 LuaJIT 返回形式。FFI、`popen`、`os.execute` 和主动编译字符串不是完整沙箱保证，详见 [File 与启动配置](file-launch.md)。

部署或升级前可查看[版本与兼容性变更](../../CHANGELOG.md)。启动无画面或遇到错误时，先按[诊断指南](debug-console.md)从终端获取日志；自动化使用 `--no-error-dialog` 禁用原生启动错误框。

关闭程序，修改根 `main.lua` 或 `example/` 下的 Lua/HLSL，保存后重新运行即可；示例没有自动热重载。只有源码开发者要改变编译期入口、授权或包记录时，才编辑唯一项目 JSON 并重新构建。操作方法见 [启动配置指南](file-launch.md)。Shader 的内置 include 为 `Engine.hlsl`。

源码普通构建只刷新对应 `build/<配置>/bin` 中的 EXE、匹配的 DXC DLL、示例与许可，不写入正式发行目录。单独更新 DLL 后再次构建也会更新部署，无需重链接 EXE。Debug 和自定义项目 Release 都不能发布标准包。标准发行使用独立目录和公开默认配置：

```powershell
.\tools\build.ps1 -Configuration Release -PublicRelease -StageRelease
```

这会在 build/public-release 构建并显式刷新 `releases/selen2d-v<构建版本>-windows-x64` 和同名 ZIP，版本来自项目构建配置。私有 launch/launch.json 不会影响标准包；公开配置禁止 secret 或游戏专用挂载。它打包固定普通文件示例，不会自动收集游戏资源包或业务目录。

构建目录中的同名示例会被覆盖，请解压 ZIP 到独立目录后开发自己的 Lua。引擎仓库的示例模板位于 `src/res/development/`；维护发行示例时修改模板。

可以先修改文件开头的设置：

| 设置 | 效果 |
|---|---|
| `width`、`height` | 初始窗口客户区大小 |
| `speed` | 每秒 60 个逻辑步时移动的场景坐标距离 |
| `seed` | 独立游戏 RNG 的种子，相同种子重现初始布局 |
| `motes` | 初始漂浮光点数量 |

示例逻辑入口先加载菜单 UI 的字体与白色像素，再注册帧回调；开始游戏时用 Loader 异步整批加载图片、铃声和 Shader。输入、显式 Object 更新与绘制都在帧回调中执行；绘制函数不能直接放在入口顶层或退出回调中。示例每个回调推进一个逻辑步，目标 60 FPS，不追赶卡顿期间的步数；暂停时不推进场景或 Shader 时间。返回菜单会销毁本轮对象、播放器与 Canvas，并卸载游戏资源，支持再次进入。

`dofile` 每次执行都重新读取文件；`File.RunLuaFile` 也可执行脚本，但丢弃脚本返回值，成功返回 `true, nil`，正常读取失败返回 `false, err`；语法、执行或授权错误仍抛错。`require('example.main')` 会走相同的脚本文件服务，不过受 `package.loaded` 缓存影响。默认搜索路径是 `?.lua;?/init.lua`，不会自动加入当前脚本目录。

请把 `doc/api` 配给 Lua 编辑器作为声明目录，运行代码直接使用引擎全局模块。声明文件中的空函数不是实现，执行它们会覆盖真实接口。
