# 开始运行与修改 Lua

将发行 ZIP 解压到一个独立项目目录，再运行其中的 `Selen2D.exe`。默认发行包只提供 EXE、启动脚本、示例和文档；不放入 Pack 工具、构建元数据、DLL 或预先生成的日志。许可证随文档提供。

```text
Selen2D.exe
launch.lua
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

`launch.lua` 只有一行：

```lua
dofile('example/main.lua')
```

双击 EXE 即可运行。WASD 或方向键移动圆形，Esc 退出；标题栏也显示操作方式。示例保留 `Window.GetTitle()` 返回的基础标题，再追加自己的说明，不读取构建元数据。

项目根始终是 EXE 所在目录。无论从哪个终端目录或快捷方式启动，`dofile('example/main.lua')`、`File.Open`、资源路径都使用这个根；进入 `example/main.lua` 不会把根改为 `example/`。因此示例通过 `Resource.LoadShader('example.effect', 'example/effect.hlsl')` 加载 Shader。

默认启动不读取外部 `launch` JSON，也不要求 mount、pkg、key、SDK、tools 目录或 Python。`launch.lua` 缺失时启动会报告路径，不会改用其他配置。这里描述的是引擎 File、资源和脚本加载接口；Lua 的 `io`、`os`、`ffi` 不属于这套虚拟路径接口。

关闭程序，修改 EXE 旁的 `example/main.lua`，保存后重新运行即可。修改 Lua 或 HLSL 不需要重新编译 C++；示例也没有自动热重载。

源码普通构建只刷新对应 `build/<配置>/bin` 中的引擎、启动脚本和示例，不写入正式发行目录。Debug 不能发布正式包。先完成 Release 构建，再显式运行发布目标：

```powershell
.\tools\build.ps1 -Configuration Release
cmake --build --preset selen2d-release --target stage_release
```

只有第二步会刷新 `releases/selen2d-v2.0.0-windows-x64` 并生成同名 ZIP。它也会更新 Release 的本地示例；Debug 发布请求及直接以 Debug 配置调用发布脚本都会报错。不同配置的本地构建目录互相独立。

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

`dofile` 每次执行都重新读取文件；`File.RunLuaFile` 也可执行脚本，但丢弃脚本返回值，读失败返回 `false`。`require('example.main')` 会走相同的脚本文件服务，不过受 `package.loaded` 缓存影响。默认搜索路径是 `?.lua;?/init.lua`，不会自动加入当前脚本目录。

请把 `doc/api` 配给 Lua 编辑器作为声明目录，运行代码直接使用引擎全局模块。声明文件中的空函数不是实现，执行它们会覆盖真实接口。
