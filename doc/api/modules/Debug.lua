---@meta

-- Selen2D 二号机已实现声明；仅供编辑器，不应执行。
-- 来源：lua_bind/modules/Debug、core/modules/Debug、core/runtime/Log；设计依据：二号机/modules/Debug.md。
-- 参数数量和类型错误抛 Lua 错误；日志不隐式 tostring。弹窗是 SDL 主线程同步阻塞操作。
---@alias DebugDialogType 'info'|'warning'|'error'
---@class DebugModule
Debug = {}

---同步记录 Info 日志，无返回值。EXE 同目录 engine.log 在启动时覆盖；不请求退出。
---@param message string 允许包含 NUL 的 Lua 字符串。
function Debug.Log(message) end

---同步记录 Warn 日志，无返回值；不请求退出或自动弹窗。
---@param message string
function Debug.Warn(message) end

---记录 Error 日志后立即抛出含原消息的 Lua 错误，没有正常返回路径，不自动弹窗。
---pcall/xpcall/coroutine 可以捕获；捕获后可继续后续帧，不额外设置退出请求。
---未捕获错误到达宿主边界才安全关闭；不会取消此前独立的 Runtime.Quit 请求。
---@param message string
function Debug.Error(message) end

---Windows 显示或隐藏引擎独立调试控制台；正常 GUI 子系统启动时默认不存在控制台，不闪黑框。
---true 在必要时创建，false 不创建；重复调用安全，无返回值。只操作引擎创建的窗口。
---不附着调用者终端，不重绑定 stdin/stdout/stderr；重定向与 engine.log 继续有效。
---必须在 SDL 主线程。非布尔参数、创建失败或已附着外部控制台时抛错，不操作外部窗口。
---非 Windows：false 安全空操作，true 抛“不支持”错误；日志 API 仍正常工作。
---@param visible boolean
function Debug.SetConsoleVisible(visible) end

---查询引擎自己创建的控制台窗口当前是否可见，不表示调用者的终端是否存在。
---不接受参数；必须在 SDL 主线程。尚未创建、已隐藏及非 Windows 均为 false。
---@return boolean visible
function Debug.IsConsoleVisible() end

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
