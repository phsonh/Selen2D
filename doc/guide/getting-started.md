# 开始运行与修改 Lua

将发行 ZIP 解压到一个独立项目目录，再运行其中的 `Selen2D.exe`。默认发行包只提供 EXE、示例和文档；不放入 Python 打包工具、启动头文件、构建元数据、DLL 或预先生成的日志。许可证随文档提供。这里描述当前 Windows 开发示例，不代表全部二号机模块或其他平台已经完成验收。

```text
Selen2D.exe
example/
  main.lua
  effect.hlsl
  palette.hlsl
doc/
  api/
    modules/  -- Audio.lua、Graphic.lua 等功能模块声明
    runtime/  -- Runtime.lua
  guide/
    LICENSE
    THIRD_PARTY_NOTICES.txt
    licenses/
```

启动配置已编译在 EXE 内。默认配置来自源码 `src/res/development/launch.h.in`，入口是 `example/main.lua`，没有资源包或 secret，真实目录授权是 `example → example` 和 `output → output`。运行时不读取 EXE 旁的 `launch.lua`、JSON 或 `launch.h`，也没有失败后改读外部配置的备用路径。

双击 EXE 即可运行。WASD 或方向键移动圆形，Esc 退出；标题栏也显示操作方式。示例保留 `Window.GetTitle()` 返回的基础标题，再追加自己的说明，不读取构建元数据。

Lua 使用相对 VFS 根的虚拟路径。默认配置将 `example/...` 映射到 EXE 旁的 `example/...`，`output/...` 映射到 EXE 旁的 `output/...`；其他部署文件不会自动获准访问。从哪个终端目录或快捷方式启动、当前执行哪个脚本，都不会改变这组映射。因此示例通过 `Resource.LoadShader('example.effect', 'example/effect.hlsl')` 加载 Shader，`dofile('example/other.lua')` 也保持完整虚拟路径。

默认运行不需要资源包、SDK、tools 目录或 Python。缺少 `example/main.lua` 会导致启动失败。`output/` 可以不存在，启动不会自动创建；需要写入时先调用 `File.CreateDirectory('output')`。标准 Lua 的文件名形式 `io.open/input/output/lines`、`loadfile/dofile` 和 `os.remove/rename` 也经过 VFS，仍保持各自的 LuaJIT 返回形式。FFI、`popen`、`os.execute` 和主动编译字符串不是完整沙箱保证，详见 [File 与启动配置](file-launch.md)。

关闭程序，修改 EXE 旁的 `example/main.lua`，保存后重新运行即可。在默认授权不变时，修改 Lua 或 HLSL 不需要重新编译 C++；示例没有自动热重载。更改入口、授权目录或包记录需要更改构建期 `launch.h` 并重新构建；操作方法见 [启动配置指南](file-launch.md)。Shader 的内置 include 仍为 `Solongo.hlsl`。

源码普通构建只刷新对应 `build/<配置>/bin` 中的引擎与示例，不写入正式发行目录。Debug 不能发布正式包。先完成 Release 构建，再显式运行发布目标：

```powershell
.\tools\build.ps1 -Configuration Release
cmake --build --preset selen2d-release --target stage_release
```

只有第二步会刷新 `releases/selen2d-v<构建版本>-windows-x64` 并生成同名 ZIP，版本来自项目构建配置。它也会更新 Release 的本地示例；Debug 发布请求及直接以 Debug 配置调用发布脚本都会报错。不同配置的本地构建目录互相独立。这个发布目标打包的是固定开发示例，不会根据私有启动配置自动收集游戏资源包或业务目录。

构建目录中的同名示例会被覆盖，请解压 ZIP 到独立目录后开发自己的 Lua。引擎仓库的示例模板位于 `src/res/development/`；维护发行示例时修改模板。

可以先修改文件开头的设置：

| 设置 | 效果 |
|---|---|
| `width`、`height` | 初始窗口客户区大小 |
| `moveSpeed` | 移动速度，单位为每秒多少个窗口高度 |
| `radius` | 圆形半径，建议从 `0.01` 到 `0.30` 调整 |
| `accentColor` | 三个 RGB 数值，例如 `{1.0, 0.35, 0.15}` |
| `backgroundColor` | 背景 RGB 数值，示例使用 `0..1` 范围 |

入口先加载资源，再用 `Runtime.SetFrameFunction` 注册帧回调。输入、移动和 `Graphic.Render` 都在帧回调中执行；绘制函数不能直接放在入口顶层或退出回调中。示例按真实帧间隔移动，并限制拖动窗口后单次移动距离。

`dofile` 每次执行都重新读取文件；`File.RunLuaFile` 也可执行脚本，但丢弃脚本返回值，成功返回 `true, nil`，正常读取失败返回 `false, err`；语法、执行或授权错误仍抛错。`require('example.main')` 会走相同的脚本文件服务，不过受 `package.loaded` 缓存影响。默认搜索路径是 `?.lua;?/init.lua`，不会自动加入当前脚本目录。

请把 `doc/api` 配给 Lua 编辑器作为声明目录，运行代码直接使用引擎全局模块。声明文件中的空函数不是实现，执行它们会覆盖真实接口。
