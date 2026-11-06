# 日志与 Windows 启动诊断

- 在终端查看：PowerShell 运行 `& .\Selen2D.exe 2>&1 | Out-Host`；CMD 可用 `start /wait "" Selen2D.exe` 等待退出。
- 分别保存两路输出：CMD 运行 `start /wait "" Selen2D.exe 1>stdout.txt 2>stderr.txt`。文件由宿主显式创建，引擎不自动写 `engine.log`。
- `Debug.GetLog()` 返回有界的 Lua 日志与 Lua 错误快照，不包含普通原生初始化和关闭日志；它不清空记录，也不写文件。
- 正常双击不弹控制台。无输出接收方的交互启动遇到原生启动失败时，可能显示一次错误框；自动化传 `--no-error-dialog` 禁用。
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

`Debug.Log(message)` 不请求退出；`Debug.Warn(message)` 不自动弹窗；`Debug.Error(message)` 记录后抛出原消息的 Lua 错误，仍可用 `pcall` 捕获。未捕获错误在宿主边界报告并正常清理，原有非零退出码不变。

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

## 原生启动失败提示

错误框只用于导致启动终止的原生故障，例如日志初始化、命令行参数、编译期配置解析、VFS 初始化或入口文件读取/来源校验、DXC 加载和原生模块初始化失败。它不用于已进入 Lua 执行的入口错误、`Debug.Error()`、帧/退出回调错误、运行时资源加载失败或异步任务错误。这些诊断继续按现有标准流与 Lua 错误契约处理。

默认只有同时满足以下条件才允许显示：已确认是交互桌面；没有实际 stdout/stderr 输出路由；没有关联控制台；没有调试器。路由判断包含引擎保留的 fallback 句柄、NUL 和 broken pipe：不能因为它们没有可见文字就替换路由或弹框。条件无法确认时不弹，后台服务和非交互运行不应因此等待用户点击。

提示在主线程完成必要清理后至多显示一次，不自动创建控制台或日志文件，不写入 `Debug.GetLog()`，不改变原有非零退出码。完整诊断仍尝试输出到标准流或调试器。

原生服务初始化失败记录具体模块/操作及失败当时取得的错误详情。即使后续 SDL 或资源清理改变了最后错误，提示仍保留原始原因；不从 Lua 日志快照中提取原生错误。

自动化可以显式禁用所有这种启动提示：

```powershell
.\Selen2D.exe --no-error-dialog
```

该选项只在第一个独立的 `--` 之前解释；例如 `Selen2D.exe -- --no-error-dialog` 将它留作应用参数，不禁用提示。`Runtime.GetArguments()` 保留收到的全部参数，不因宿主识别这个选项而移除内容。

DXC 失败诊断包含期望的绝对 DLL 路径及 Win32 错误码。引擎只从 EXE 目录调用系统加载器并解析 `DxcCreateInstance`；不会从当前目录、PATH 或 SDK 回退。**运行时不校验 DLL 版本号或 SHA256**：固定版本、x64 格式、依赖白名单和文件摘要检查属于构建/打包步骤。能加载且具有导出的替换 DLL 不会因此被运行时拒绝，应部署随同 EXE 构建的 DLL。详情见 [Shader 指南](shader.md)。

## 高级：标准句柄和控制事件

引擎优先使用宿主传入的有效标准句柄。启动时若 stdout 或 stderr 缺失，仅尝试一次 `AttachConsole(ATTACH_PARENT_PROCESS)`，由 Windows 检查并连接父进程已经存在的终端；父进程没有控制台时失败，启动照常继续，不调用 `AllocConsole()`。附着后保留原有有效的文件、管道、NUL 重定向及 stdin，不更改调用者的窗口或编码设置。文件和管道接收 UTF-8 字节；可用的 Windows 控制台句柄接收 Unicode。正常双击启动不创建控制台窗口；原生启动终止时是否显示错误框，按上面的条件判断。

使用 `STARTF_USESTDHANDLES` 启动时，Attach 成功不代表非 NULL 的无效输出句柄已被替换。引擎恢复有效的原路由后会再次验证两路输出，只为原本无效且仍无效的一路打开已附着控制台的 `CONOUT$` 并设置标准句柄。两路需要修复时共用一个新句柄，最多打开一次；它保持到进程退出，由 Windows 回收，不在 Shutdown 时关闭，保证 reInit 和关闭后的原生日志仍可输出。调用者传入的有效句柄始终是借用的，不由引擎关闭。参见 [GetStdHandle 的 Attach/detach behavior](https://learn.microsoft.com/en-us/windows/console/getstdhandle#attachdetach-behavior)。

附着成功后才安装内部控制处理器，并仅对本进程清除继承的“忽略 Ctrl+C”属性。Ctrl+C 和 Ctrl+Break 仅设置退出请求，由主线程在安全边界处理：当前同步初始化、Lua 回调或原生操作自然返回后，不再开始下一普通帧，按原顺序执行已注册的 ExitFunction（最多一次）和引擎清理。正常清理返回 0；初始化、Lua 或退出清理已有错误仍返回原来的非零退出码。信号不能中断无限循环的 Lua、长期阻塞的 I/O 或驱动调用。初始化引擎之前收到并消费请求时，不执行尚未注册的 Lua 回调；关闭过程中再次收到请求也不会重入清理。处理器保留到进程结束，日志关闭不卸载它。附着和处理器安装之前的控制事件不保证被捕获。

两路输出原本均有效时不附着，也不改变宿主已有控制事件行为。附着失败同样不安装处理器。若附着后的控制处理器安装失败，或恢复任一路标准句柄失败，引擎输出明确诊断并以启动错误码 2 退出，不在错误的 stdin/重定向状态下继续游戏。恢复失败的有效 stdout/stderr 仍供日志借用原句柄输出；引擎不会关闭它们、重绑 CRT 或退回另一输出路由。broken pipe 保持为原管道，写失败不会改投控制台。

成功附着后的 `CONOUT$` 打开、输出修复或最终句柄验证失败也采用错误码 2，每个失败操作只报告一次，包含 Win32 错误码且不进入 Lua 日志缓存。尽可能通过仍有效的原输出或已打开的控制台句柄诊断；两路都不可用时仍向调试器输出诊断，原生启动错误框另按前述条件判断；不声称终端输出已恢复。没有父控制台的 Attach 失败仍静默继续，不因此中止 GUI 启动。

**关闭终端不同于 Ctrl+C。** Windows 的 CTRL_CLOSE_EVENT 可强制终止控制台客户端，即使处理器返回成功也不保证主线程完成退出。引擎不拦截这一事件，也不在控制线程调用 Lua 或资源清理；关闭父终端、任务管理器结束进程等情况不保证 ExitFunction、存档及最后的原生日志完成。父 shell 单独退出不等于销毁控制台，是否继续运行取决于宿主是否保留终端。参见 [Windows 控制处理器](https://learn.microsoft.com/windows/console/setconsolectrlhandler) 和 [CTRL_CLOSE_EVENT](https://learn.microsoft.com/en-us/windows/console/ctrl-close-signal)。

这些 Windows 路由和交互桌面规则的实测范围为 Windows x64。其他平台没有在本版本完成构建与运行验收。
