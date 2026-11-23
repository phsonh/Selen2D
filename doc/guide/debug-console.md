# 日志与 Windows 启动诊断

- 在终端查看：PowerShell 运行 `& .\Selen2D.exe 2>&1 | Out-Host`；CMD 可用 `start /wait "" Selen2D.exe` 等待退出。
- 分别保存两路输出：CMD 运行 `start /wait "" Selen2D.exe 1>stdout.txt 2>stderr.txt`。文件由宿主显式创建，引擎不自动写 `engine.log`。
- `Debug.GetLog()` 返回有界的 Lua 日志与 Lua 错误快照，不包含普通原生初始化和关闭日志；它不清空记录，也不写文件。
- 正常双击不弹控制台。交互桌面遇到导致运行失败的未处理错误时，stderr 诊断与一次错误框同时提供；自动化传 `--no-error-dialog` 只禁用弹窗。
- 完整生命周期诊断走 stdout（Info/Debug）和 stderr（Warn/Error/Fatal），并保留 Windows 调试器输出。

## 日常查看和重定向

引擎使用 Windows GUI 子系统，不创建独立控制台。现有 `engine.log` 不会被清空或覆盖；需要保存时由宿主重定向，或由游戏主动保存 Lua 快照。运行目录应保留同配置的 `Selen2D.exe` 与 `dxcompiler.dll`。

GUI 程序在某些 shell 中不会自动等待退出。CMD 直接运行时提示符可能与日志交错，这不等于丢失日志。需要顺序显示及可靠退出码时使用 `start /wait "" Selen2D.exe`；自动化也可使用 PowerShell 的 `Start-Process -FilePath .\Selen2D.exe -ArgumentList '--no-error-dialog' -Wait -PassThru` 配合 `-RedirectStandardOutput` / `-RedirectStandardError`，或 Python `subprocess.run(['Selen2D.exe', '--no-error-dialog'], capture_output=True)`。

若宿主既没有传入有效标准句柄、父进程也不拥有控制台，附着父进程不能产生输出。例如直接把 GUI 进程作为 ConPTY-root，或某些 VS Code 直接 GUI 进程任务。可以使用 shell 任务，或明确使用以下 PowerShell 管道任务（只在任务宿主合并显示两路输出，引擎内部仍分别写 stdout/stderr）：

```json
{
  "version": "2.0.0",
  "tasks": [{
    "label": "Run Selen2D",
    "type": "process",
    "command": "powershell.exe",
    "args": ["-NoProfile", "-Command",
      "& $env:SELEN2D_RUN_EXE --no-error-dialog 2>&1 | Out-Host; exit $LASTEXITCODE"],
    "options": {"env": {"SELEN2D_RUN_EXE": "${workspaceFolder}/Selen2D.exe"}},
    "problemMatcher": []
  }]
}
```

CMD 可使用 `start /wait "" Selen2D.exe 1>stdout.txt 2>stderr.txt` 捕获分流；PowerShell 直接查看时可使用 `& .\Selen2D.exe 2>&1 | Out-Host`。这些显式重定向文件由宿主创建，不是引擎自动保存。

## Lua 日志快照与主动保存

`Debug.Log(...)` 接收任意数量的参数，按当前全局 `tostring` 从左到右转换，以制表符（`\t`）分隔为一条 Info 日志，写入 stdout 和 Lua 日志缓存，无返回值。支持数字、布尔值、nil、表、userdata 及 `__tostring`；字符串内的 NUL 保留。空参数调用记录一条空正文日志。转换失败时原 Lua 错误继续向外传播，本次调用不写出部分日志；转换函数自己产生的副作用保留。

引擎注册 Debug 时将全局 `print` 指向同一个函数，因此 `print == Debug.Log`，两者都带日志时间戳和 Info 级别，并进入 `Debug.GetLog()`。这是初始化时赋值，后续自行替换其中一个变量不会自动同步另一个。

```lua
Debug.Log("场景帧数：", 120)
print("加载完成", true, nil) -- 正文为：加载完成\ttrue\tnil
```

`Debug.Log`/`print` 不请求退出；`Debug.Warn(message)` 仍只接受一个字符串且不自动弹窗；`Debug.Error(message)` 仍记录后抛出原消息的 Lua 错误，可用 `pcall` 捕获。未捕获错误在宿主边界报告并正常清理，原有非零退出码不变。

`Debug.GetLog()` 无参数，返回当前进程中由 Lua 日志 API 以及宿主报告的 Lua 错误产生的快照。它不包括 SDL、D3D12、Audio 等普通初始化日志，也不包括 Lua VM 卸载后的原生日志。获取不清空记录、不访问磁盘、不改变日志级别。保留时间戳、级别及消息字节，包括 Lua 字符串中的 NUL。

缓存最多保存 1 MiB 格式化日志正文及 8192 条记录；超限淘汰最旧记录，结果开头带截断提示。单条超大消息只保留头部及能容纳的正文尾部并标明截断。有效 UTF-8 不在截断点拆分字符。容量限制只影响内存快照，标准输出仍接收完整消息。获取结果和 Lua 返回字符串占用额外的调用者内存。

游戏可以主动保存快照。以下目录必须已经处于项目的 VFS 可写授权内；引擎不会为日志增加授权：

```lua
Runtime.SetExitFunction(function()
    local ok, err = File.CreateDirectory("playerdata/logs")
    if not ok then Debug.Warn(err.message); return end
    local file, openErr = File.Open("playerdata/logs/engine.log", "wb")
    if not file then Debug.Warn(openErr.message); return end
    local written, writeErr = file:Write(Debug.GetLog())
    local closed, closeErr = file:Close()
    if not written or not closed then Debug.Warn((writeErr or closeErr).message) end
end)
```

授权/参数错误仍按 File 契约抛错，正常 I/O 失败仍返回错误表。需要自行处理授权错误的游戏可使用 `pcall`。保存失败不会触发引擎自动保存或递归重试。

快照只能包含获取时已经产生的消息。ExitFunction 自己随后抛出的错误、之后的清理日志不可能出现在先前的快照里。入口尚未注册退出函数就失败、崩溃、强制终止均不保证执行保存回调；不为此改变退出顺序。这些诊断仍写往可用的标准流及调试器。

## 未处理错误提示

2.3.5 起，交互桌面上导致本次运行失败的未处理错误会在写入 stderr 后显示一次错误框：包含原生启动/配置/DXC/服务初始化、Lua 入口加载与执行、帧回调、退出回调、Object 收尾回调，以及宿主已接管的运行期致命错误。有控制台、stdout/stderr 重定向、NUL、broken pipe 或调试器均不禁止提示，也不改变原有输出路由。

宿主保存第一项实际失败的阶段、原始详情及可用 Lua traceback；后续清理错误只追加，不覆盖首错。错误及时输出，主线程完成可安全执行的既有清理后统一弹一次；多层 File.RunLuaFile 传播不会产生多个框。额外清理说明有界（最多64KiB，超出标明省略），各条标准错误输出不因此被截断。弹框/编码失败不覆盖原错误、不改变退出码，不读取 Debug.GetLog 来判断失败。

`pcall` / `xpcall` 已处理的异常、普通API的 `false/nil,err` 返回及非致命日志不触发宿主失败。`Debug.Error` 仍是记录后抛错：捕获时只保留日志；未捕获到宿主并导致运行失败时参与统一提示。正常 Quit、关闭窗口、正常退出不弹错误框。

只在确认活动交互会话、可见窗口站及当前可交互桌面时显示；无法确认或非交互运行不阻塞等待。Windows 仍是GUI子系统，不自动创建控制台或engine.log。Lua日志收录范围、标准句柄、Ctrl+C/Ctrl+Break、退出顺序均保持。

自动化显式禁用提示（不改变抛错、stderr或非零退出码）：

```powershell
.\Selen2D.exe --no-error-dialog
```

该选项只在第一个独立的 `--` 之前解释；例如 `Selen2D.exe -- --no-error-dialog` 将它留作应用参数，不禁用提示。`Runtime.GetArguments()` 保留收到的全部参数，不因宿主识别这个选项而移除内容。

DXC 失败诊断包含期望的绝对 DLL 路径及 Win32 错误码。引擎只从 EXE 目录调用系统加载器并解析 `DxcCreateInstance`；不会从当前目录、PATH 或 SDK 回退。**运行时不校验 DLL 版本号或 SHA256**：固定版本、x64 格式、依赖白名单和文件摘要检查属于构建/打包步骤。能加载且具有导出的替换 DLL 不会因此被运行时拒绝，应部署随同 EXE 构建的 DLL。详情见 [Shader 指南](shader.md)。

## 高级：标准句柄和控制事件

引擎优先使用宿主传入的有效标准句柄。启动时若 stdout 或 stderr 缺失，仅尝试一次 `AttachConsole(ATTACH_PARENT_PROCESS)`，由 Windows 检查并连接父进程已经存在的终端；父进程没有控制台时失败，启动照常继续，不调用 `AllocConsole()`。附着后保留原有有效的文件、管道、NUL 重定向及 stdin，不更改调用者的窗口或编码设置。文件和管道接收 UTF-8 字节；可用的 Windows 控制台句柄接收 Unicode。正常双击启动不创建控制台窗口；未处理错误导致运行失败时是否显示错误框，按上面的条件判断。

使用 `STARTF_USESTDHANDLES` 启动时，Attach 成功不代表非 NULL 的无效输出句柄已被替换。引擎恢复有效的原路由后会再次验证两路输出，只为原本无效且仍无效的一路打开已附着控制台的 `CONOUT$` 并设置标准句柄。两路需要修复时共用一个新句柄，最多打开一次；它保持到进程退出，由 Windows 回收，不在 Shutdown 时关闭，保证 reInit 和关闭后的原生日志仍可输出。调用者传入的有效句柄始终是借用的，不由引擎关闭。参见 [GetStdHandle 的 Attach/detach behavior](https://learn.microsoft.com/en-us/windows/console/getstdhandle#attachdetach-behavior)。

附着成功后才安装内部控制处理器，并仅对本进程清除继承的“忽略 Ctrl+C”属性。Ctrl+C 和 Ctrl+Break 仅设置退出请求，由主线程在安全边界处理：当前同步初始化、Lua 回调或原生操作自然返回后，不再开始下一普通帧，按原顺序执行已注册的 ExitFunction（最多一次）和引擎清理。正常清理返回 0；初始化、Lua 或退出清理已有错误仍返回原来的非零退出码。信号不能中断无限循环的 Lua、长期阻塞的 I/O 或驱动调用。初始化引擎之前收到并消费请求时，不执行尚未注册的 Lua 回调；关闭过程中再次收到请求也不会重入清理。处理器保留到进程结束，日志关闭不卸载它。附着和处理器安装之前的控制事件不保证被捕获。

两路输出原本均有效时不附着，也不改变宿主已有控制事件行为。附着失败同样不安装处理器。若附着后的控制处理器安装失败，或恢复任一路标准句柄失败，引擎输出明确诊断并以启动错误码 2 退出，不在错误的 stdin/重定向状态下继续游戏。恢复失败的有效 stdout/stderr 仍供日志借用原句柄输出；引擎不会关闭它们、重绑 CRT 或退回另一输出路由。broken pipe 保持为原管道，写失败不会改投控制台。

成功附着后的 `CONOUT$` 打开、输出修复或最终句柄验证失败也采用错误码 2，每个失败操作只报告一次，包含 Win32 错误码且不进入 Lua 日志缓存。尽可能通过仍有效的原输出或已打开的控制台句柄诊断；两路都不可用时仍向调试器输出诊断，错误框另按前述条件判断；不声称终端输出已恢复。没有父控制台的 Attach 失败仍静默继续，不因此中止 GUI 启动。

**关闭终端不同于 Ctrl+C。** Windows 的 CTRL_CLOSE_EVENT 可强制终止控制台客户端，即使处理器返回成功也不保证主线程完成退出。引擎不拦截这一事件，也不在控制线程调用 Lua 或资源清理；关闭父终端、任务管理器结束进程等情况不保证 ExitFunction、存档及最后的原生日志完成。父 shell 单独退出不等于销毁控制台，是否继续运行取决于宿主是否保留终端。参见 [Windows 控制处理器](https://learn.microsoft.com/windows/console/setconsolectrlhandler) 和 [CTRL_CLOSE_EVENT](https://learn.microsoft.com/en-us/windows/console/ctrl-close-signal)。

这些 Windows 路由和交互桌面规则的实测范围为 Windows x64。其他平台没有在本版本完成构建与运行验收。
