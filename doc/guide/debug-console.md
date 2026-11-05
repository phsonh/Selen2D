# 日志与 Windows 启动诊断

引擎使用 Windows GUI 子系统，不创建控制台窗口。Info/Debug 实时输出到 stdout，Warn/Error/Fatal 实时输出到 stderr，包含启动验证、Lua 运行及 Lua VM 卸载后的原生清理日志。Windows 调试器仍可接收诊断镜像。引擎不创建、覆盖或写入 engine.log，已有同名文件保持原样。

引擎优先使用宿主传入的有效标准句柄。启动时若 stdout 或 stderr 缺失，仅尝试一次 `AttachConsole(ATTACH_PARENT_PROCESS)`，由 Windows 检查并连接父进程已经存在的终端；父进程没有控制台时失败，启动照常继续，不调用 `AllocConsole()`。附着后保留原有有效的文件、管道、NUL 重定向及 stdin，不更改调用者的窗口或编码设置。文件和管道接收 UTF-8 字节；可用的 Windows 控制台句柄接收 Unicode。双击启动没有接收方时，不会创建窗口或弹出阻塞错误框；可用终端重新运行或连接调试器诊断。

附着成功后才安装内部控制处理器，并仅对本进程清除继承的“忽略 Ctrl+C”属性。Ctrl+C 和 Ctrl+Break 仅设置退出请求，由主线程在安全边界处理：当前同步初始化、Lua 回调或原生操作自然返回后，不再开始下一普通帧，按原顺序执行已注册的 ExitFunction（最多一次）和引擎清理。正常清理返回 0；初始化、Lua 或退出清理已有错误仍返回原来的非零退出码。信号不能中断无限循环的 Lua、长期阻塞的 I/O 或驱动调用。初始化引擎之前收到并消费请求时，不执行尚未注册的 Lua 回调；关闭过程中再次收到请求也不会重入清理。处理器保留到进程结束，日志关闭不卸载它。附着和处理器安装之前的控制事件不保证被捕获。

两路输出原本均有效时不附着，也不改变宿主已有控制事件行为。附着失败同样不安装处理器。若附着后的控制处理器安装失败，或恢复任一路标准句柄失败，引擎输出明确诊断并以启动错误码 2 退出，不在错误的 stdin/重定向状态下继续游戏。恢复失败的有效 stdout/stderr 仍供日志借用原句柄输出；引擎不会关闭它们、重绑 CRT 或退回另一输出路由。broken pipe 保持为原管道，写失败不会改投控制台。

**关闭终端不同于 Ctrl+C。** Windows 的 CTRL_CLOSE_EVENT 可强制终止控制台客户端，即使处理器返回成功也不保证主线程完成退出。引擎不拦截这一事件，也不在控制线程调用 Lua 或资源清理；关闭父终端、任务管理器结束进程等情况不保证 ExitFunction、存档及最后的原生日志完成。父 shell 单独退出不等于销毁控制台，是否继续运行取决于宿主是否保留终端。参见 [Windows 控制处理器](https://learn.microsoft.com/windows/console/setconsolectrlhandler) 和 [CTRL_CLOSE_EVENT](https://learn.microsoft.com/en-us/windows/console/ctrl-close-signal)。

GUI 程序在某些 shell 中不会自动等待退出。CMD 直接运行时提示符可能与日志交错，这不等于丢失日志。需要顺序显示及可靠退出码时使用 `start /wait "" Selen2D.exe`；自动化也可使用 PowerShell 的 `Start-Process -Wait -PassThru` 配合 `-RedirectStandardOutput` / `-RedirectStandardError`，或 Python `subprocess.run(..., capture_output=True)`。

若宿主既没有传入有效标准句柄、父进程也不拥有控制台，附着父进程不能产生输出。例如直接把 GUI 进程作为 ConPTY-root，或某些 VS Code 直接 GUI 进程任务。可以使用 shell 任务，或明确使用以下 PowerShell 管道任务（只在任务宿主合并显示两路输出，引擎内部仍分别写 stdout/stderr）：

```json
{
  "version": "2.0.0",
  "tasks": [{
    "label": "Run Selen2D",
    "type": "process",
    "command": "powershell.exe",
    "args": ["-NoProfile", "-Command",
      "& $env:SELEN2D_RUN_EXE 2>&1 | Out-Host; exit $LASTEXITCODE"],
    "options": {"env": {"SELEN2D_RUN_EXE": "${workspaceFolder}/Selen2D.exe"}},
    "problemMatcher": []
  }]
}
```

CMD 可使用 `start /wait "" Selen2D.exe 1>stdout.txt 2>stderr.txt` 捕获分流；PowerShell 直接查看时可使用 `& .\Selen2D.exe 2>&1 | Out-Host`。这些显式重定向文件由宿主创建，不是引擎自动保存。

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

非 Windows 平台使用标准输出/错误流，Lua 缓存语义相同；平台运行验证范围以开发记录为准。
