---@meta

-- Selen2D Debug 声明；仅供编辑器，不应执行。
-- 来源：lua_bind/modules/Debug、core/modules/Debug、core/runtime/Log。
-- 参数数量和类型错误抛 Lua 错误；日志不隐式 tostring。弹窗是 SDL 主线程同步阻塞操作。
---@alias DebugDialogType 'info'|'warning'|'error'
---@class DebugModule
Debug = {}

---同步记录 Info 日志到 stdout 和本次运行的 Lua 日志缓存；不自动写文件、不请求退出。
---@param message string 允许包含 NUL 的 Lua 字符串。
function Debug.Log(message) end

---同步记录 Warn 日志到 stderr 和 Lua 日志缓存，无返回值；不请求退出或自动弹窗。
---@param message string
function Debug.Warn(message) end

---记录 Error 日志后立即抛出含原消息的 Lua 错误，没有正常返回路径，不自动弹窗。
---pcall/xpcall/coroutine 可以捕获；捕获后可继续后续帧，不额外设置退出请求。
---未捕获错误到达宿主边界才安全关闭；不会取消此前独立的 Runtime.Quit 请求。
---@param message string
function Debug.Error(message) end

---获取本次进程运行的 Lua 日志快照，不接受参数；多次获取不清空、不写文件。
---包含 Debug.Log/Warn/Error 及宿主报告的 Lua 入口、帧、退出回调错误；不包含普通原生初始化和关闭日志。
---保留时间戳、级别和原始正文（包括 NUL）；有效 UTF-8 输入保持 UTF-8，不隐式 tostring。
---最多保留 1 MiB 格式化正文和 8192 条记录，超限淘汰最旧记录并在快照开头标明截断。
---单条超大日志保留头部和可容纳的正文尾部，标明截断；标准输出仍输出原消息。
---返回调用时已产生的记录。崩溃、强制终止、退出回调注册前失败不保证有机会由 Lua 保存。
---返回的字符串另占内存。可通过现有 File API 保存到已授权目录；不会扩大 VFS 权限。
---@return string text
function Debug.GetLog() end

---显示独立系统消息框，唯一按钮为“确定”；阻塞到用户选择/关闭，不在 Canvas 内绘制。
---正文/标题为 UTF-8 字符串，不得含 NUL。类型只影响图标，不请求退出。
---必须在 SDL 主线程；SDL 失败记录日志并抛错，不能当作用户关闭。窗口装饰由平台决定。
---@param message string
---@param title? string 省略或 nil 默认为“提示”。
---@param type? DebugDialogType 省略或 nil 默认为 "info"。
---@return true|nil selected 点击“确定”（包括默认 Enter）为 true，无选择关闭为 nil。
function Debug.MessageBox(message, title, type) end

---显示自定义按钮的同步独立系统消息框；返回所选按钮原文字，无选择关闭返回 nil。
---buttons 必须是非空、连续 1..n 的原始字符串数组；标签非空、唯一且无 NUL；忽略元表字段。
---正文/标题也不得含 NUL，title 必填。第一个按钮为 Enter 默认；Escape 不映射到某个按钮。
---按钮视觉排列由平台决定；类型非法、数组非法、非主线程或 SDL 显示失败均抛错。
---@param message string
---@param title string
---@param buttons string[]
---@param type? DebugDialogType 省略或 nil 默认为 "info"。
---@return string|nil selected
function Debug.ButtonBox(message, title, buttons, type) end
