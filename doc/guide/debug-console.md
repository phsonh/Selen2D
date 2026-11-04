# 调试控制台与 Windows 启动

Windows 引擎使用 GUI 子系统及 Unicode 入口。正常从资源管理器或终端启动时，不由系统先创建黑色控制台再隐藏；游戏窗口和正常启动流程不依赖控制台。

```lua
assert(not Debug.IsConsoleVisible())
Debug.Log("正常启动，控制台默认隐藏")
Debug.SetConsoleVisible(true)
Debug.Warn("现在显示引擎自己的控制台")
Debug.SetConsoleVisible(false)
Debug.Log("隐藏后仍然写日志")
```

这两个函数只管理引擎自己的诊断窗口。第一次 `true` 使用 Windows `AllocConsole` 创建它；`false` 隐藏并保留窗口及缓冲区，重复调用安全。`IsConsoleVisible()` 查询实际窗口可见状态，而不是上次设置的布尔值。诊断控制台的关闭按钮不可用，避免 Windows 直接终止进程、绕过 `Runtime` 退出清理；游戏可以用上述 API 隐藏窗口。

引擎不会 `AttachConsole` 到 PowerShell、CMD 或 Windows Terminal，也不会隐藏、关闭用户终端。创建控制台时保留原有 Win32 标准句柄，不对 CRT 文件描述符执行 `freopen` 或重绑定。日志通过独立的控制台输出句柄镜像，因此标准输出/错误的文件或管道重定向继续有效。若特殊原生宿主已经主动把进程附着到外部控制台，显示请求明确报错，而不擅自分离或改变外部终端。

GUI 程序从某些命令行直接运行时，终端可能不等待它结束。自动化应使用可等待并取得退出码的进程 API，例如 PowerShell `Start-Process -Wait -PassThru`，或直接通过 `System.Diagnostics.Process` / Python `subprocess` 启动并捕获输出。构建脚本仍是独立的命令行程序，不改变 PowerShell 本身的窗口。

`Debug.Log`、`Debug.Warn`、`Debug.Error` 的既有行为不变；`Debug.Error` 仍先记日志再抛 Lua 错误。`engine.log` 统一写在 EXE 同目录，每次启动覆盖，不随进程工作目录变化；隐藏控制台不会关闭日志。日志初始化已提前到启动配置验证之前，入口或挂载错误也写入日志及重定向的 stderr。Windows 调试器还可接收 `OutputDebugString` 输出。普通启动、Lua 回调失败和退出清理失败的非零退出码继续有效。

日志文件不可写时会记录警告；若此时没有重定向标准错误或原生调试器，不能保证磁盘上保留诊断。部署与自动化环境应允许写入 EXE 所在目录，或显式捕获标准错误。引擎内部日志路径不授予 Lua 对任意原生目录的访问权限。

参数要求：`SetConsoleVisible` 恰好一个布尔值，`IsConsoleVisible` 没有参数，两者均在 SDL 主线程调用。其他平台暂不创建原生诊断窗口：查询返回 `false`，隐藏安全空操作，显示请求抛不支持错误；标准流和文件日志照常工作。本轮实际验收平台为 Windows x64。

实现依据：[Windows console creation](https://learn.microsoft.com/en-us/windows/console/creation-of-a-console)、[AllocConsole](https://learn.microsoft.com/en-us/windows/console/allocconsole)。GUI 子系统避免默认控制台，而独立输出句柄避免破坏重定向，两者职责不同。
