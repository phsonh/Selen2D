# 日志与 Windows 启动诊断

引擎使用 Windows GUI 子系统，不自动创建或附着控制台。Info/Debug 实时输出到 stdout，Warn/Error/Fatal 实时输出到 stderr，包含启动验证、Lua 运行及 Lua VM 卸载后的原生清理日志。Windows 调试器仍可接收诊断镜像。引擎不创建、覆盖或写入 engine.log，已有同名文件保持原样。

引擎使用宿主传入的有效标准句柄，不改变调用者的窗口、句柄或编码设置。文件和管道接收 UTF-8 字节；可用的 Windows 控制台句柄接收 Unicode。没有接收方时不会创建窗口或弹出阻塞错误框。直接双击启动若失败，请用能够捕获标准流的宿主重新运行，或连接调试器。

GUI 程序在某些 shell 中不会自动等待退出。自动化应等待进程并检查退出码，例如 CMD 的 `start /wait`，PowerShell 的 `Start-Process -Wait -PassThru` 配合 `-RedirectStandardOutput` / `-RedirectStandardError`，或 Python `subprocess.run(..., capture_output=True)`。仅有终端窗口不等于宿主一定向 GUI 子进程提供了可用输出句柄；可显式使用管道或文件重定向。

本机 VS Code / ConPTY 直接以 GUI EXE 为任务进程时未提供可用的终端输出；通过 PowerShell 管道可以可靠接收完整生命周期日志。可使用以下任务配置（只在任务宿主合并显示两路输出，引擎内部仍分别写 stdout/stderr）：

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
