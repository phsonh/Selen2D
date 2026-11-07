# 开始运行与修改 Lua

本指南描述**尚未发布的 2.3.0 候选包**。目前已发布的 2.2.0 包不包含也不需要 `dxcompiler.dll`；不要为旧包补入候选版 DLL。

将候选 ZIP 完整解压到一个独立项目目录，再运行其中的 `Selen2D.exe`。这是自包含运行目录：EXE、同目录的 `dxcompiler.dll`、示例和文档一起打包，不需要另行安装编译器或运行库。包中不放入 Python 打包工具、启动头文件、构建元数据或预先生成的日志，许可证随文档提供。候选面向 Windows x64 / Direct3D 12，不代表其他平台已经完成验收。

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


<a id="upgrade-230"></a>
## 从 2.2.0 升级到 2.3.0

当前 2.3.0 是尚未发布的候选。下面汇总 2.2.1～2.2.9 开发期间已经采用的行为，帮助现有 2.2.0 项目迁移；各项首次引入版本见 [CHANGELOG](../../CHANGELOG.md)。先备份自己的 Lua、资源和存档，在独立目录验证候选包，不覆盖游戏项目来测试。

### 部署与启动

请完整解压候选包，保持同次构建、同一配置的 `Selen2D.exe` 和 `dxcompiler.dll` 相邻。2.2.3 起运行时 HLSL 编译从 EXE 目录加载配套 DLL；不从 PATH、当前工作目录或 SDK 回退。缺 DLL、系统依赖或架构不符等原生启动失败会保留具体操作、路径及系统错误。运行时只检查加载成功和 `DxcCreateInstance` 导出，不按版本号或哈希拒绝所有不匹配 DLL；部署配套文件仍是开发者的责任。

根 `main.lua`、`File.RunLuaFile("example/main.lua")`、完整 `example/` 及公开默认根映射保持不变。普通用户可以修改获准的 Lua/HLSL 并重启，无需 Python、JSON 或重新编译。源码开发者改变入口、授权或包配置时仍需重新构建；通用包继续使用隔离的公开默认配置，不带项目私有 secret。

JSON 的 `root` / `local_app_data` 名称已经在正式 2.2.0 中采用，并非 2.3.0 新增迁移。更老项目请按[旧配置迁移](file-launch.md#旧配置迁移)处理；Windows 用户数据仍解析到系统 LocalAppData。

### 日志与错误处理

2.2.2 删除了 `Debug.SetConsoleVisible()` / `Debug.IsConsoleVisible()`，没有兼容别名；请从游戏代码中移除这些调用。查看整个进程的初始化、Lua 运行和原生关闭日志，使用终端或宿主重定向，例如 CMD：

```bat
start /wait "" Selen2D.exe --no-error-dialog 1>stdout.txt 2>stderr.txt
```

Info/Debug 写 stdout，Warn/Error/Fatal 写 stderr。上述文件由 CMD 显式创建；引擎不再创建、覆盖或写入 `engine.log`。仅缺少输出句柄时，引擎尝试连接父进程已有控制台，保留有效文件、管道和 NUL 路由；没有接收方时不自动创建控制台。交互 CMD 直接启动 GUI 程序可能先返回提示符，日志会与提示符交错。

`Debug.GetLog()` 只包含本次 Lua 的 `Debug.Log/Warn/Error` 和宿主报告的 Lua 错误，**不包括普通原生初始化或 Lua VM 销毁后的清理日志**。缓存上限为 1 MiB/8192 条，超限标记截断；读取不清空、不写文件。需要保存时，在退出回调中通过已获 VFS 写入授权的 File 接口保存；此前快照不可能包含随后才产生的退出错误。完整示例及控制事件边界见[日志指南](debug-console.md)。

2.2.3 起，原生启动终止在确认交互桌面且没有输出路由、控制台或调试器时，可能在清理后显示一次错误框。普通 Lua 错误、帧/退出回调错误和运行中资源加载失败不因此弹框。自动化请在独立 `--` 之前传 `--no-error-dialog`；原有参数列表、非零退出码和退出顺序不变。

### 文件占用与安全验证

2.2.4 明确调整了无关目录独占时的结构错误发现时机。启动仍完整验证映射树。运行中只有确认是共享冲突的无关子树可以标记为待验证；目标文件、必经父目录和本次所依赖的映射仍实时检查。如果缺失信息使授权、包来源保护或跨映射文件身份无法确认，操作仍失败。

后续操作会重试待验证子树；解除占用后的首次可检查操作先完成完整结构验证，再继续。不是在解锁瞬间后台自动验证，也不把旧视图当安全证明。已观察到的名称冲突、重解析点和非法身份仍立即拒绝；权限拒绝、未知 I/O 错误不采用延期。跨操作 O(N) 扫描尚未完全消除。详见 [VFS 验证与目录占用](file-launch.md#vfs-验证与目录占用)。

### 性能改动的适用范围

| 路径 | 已采用的改动 | 保持的边界 |
|---|---|---|
| 精灵 | 精简顶点/共享参数、索引四边形、命令引用排序、相邻相同参数复用 | 原 Lua 接口、layer 稳定顺序、Canvas 屏障、预算和资源代次 |
| Lua 绑定 | 减少成功调用时诊断缓冲的初始化 | 参数检查、错误文本、动态字段及回调行为 |
| 碰撞 | 单次分发复用对象地址；在原顺序中保守筛选稀疏配对 | 首次回调之后恢复原枚举；实时几何、异常、回调顺序不变；最坏仍为 O(A×B)/O(N²) |
| 文字 | 字形 GPU 命中路径和描边/填充取得复用；512 项/32 MiB 有界内容布局缓存 | 字体身份、完整样式、绘制顺序、上传事务及在途保活；未引入 PreparedText 缓存 |

同一尚未提交的录制批次内，连续 N 条 compact 2D 精灵的采样参数完全相同时，几何上传量可从 `184*N` 字节变为 `136*N+48`。这不包含全部命令、CPU 容器和 GPU 内存；不重复参数仍是每条 184 字节。数据量减少不等于同百分比的 CPU、GPU 或 FPS 收益。现有 API 和脚本不需要为这些性能路径新增批量调用。

候选范围仅覆盖 Windows x64 / Direct3D 12 / DXIL；其他平台、图形后端和实际 THKTNT 游戏移植不在这里宣称完成。历史 2.2.8 窗口状态触发及第 71 帧尖峰仍留作未解决诊断，不因本次汇总升级而视为已修复。
