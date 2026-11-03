---@meta

-- Selen2D v2.0.0 一号机基线声明；仅供编辑器，不应执行。
-- 来源：lua_bind/modules/Debug、core/modules/Debug、core/runtime/Log；设计依据：一号机/modules/Debug.md。
-- 参数数量和类型错误抛 Lua 错误；日志不隐式 tostring。弹窗是 SDL 主线程同步阻塞操作。
---@alias DebugDialogType 'info'|'warning'|'error'
---@class DebugModule
Debug = {}

---同步记录 Info 日志，无返回值。沿用宿主日志输出，engine.log 在启动时覆盖；不请求退出。
---@param message string 允许包含 NUL 的 Lua 字符串。
function Debug.Log(message) end

---同步记录 Warn 日志，无返回值；不请求退出或自动弹窗。
---@param message string
function Debug.Warn(message) end

---记录 Error 日志并立即设置宿主退出请求，无返回值；有效调用自身不抛 Lua 错误。
---当前 Lua 代码继续执行，宿主随后在安全边界退出并调用 ExitFunction；pcall 不能取消请求。
---不会自动弹窗；参数使用错误仍抛错且不会设置退出请求。
---@param message string
function Debug.Error(message) end

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
