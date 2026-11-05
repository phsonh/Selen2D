---@meta

-- Selen2D Window 声明（14 个公开函数）；仅供编辑器，不应执行。
-- 来源：lua_bind/modules/Window、core/modules/Window、core/modules/Graphic/Graphic.cpp、engine/Engine.cpp。
-- 操作系统状态与绘制目标可用性分别处理。
-- 单一宿主窗口；所有接口须在已初始化 SDL/图形主线程调用。参数数量/类型或平台操作失败抛错。
-- Setter 无返回值；Getter 读取当前已应用状态，不推动状态变更。系统可能异步应用窗口请求。
-- 启动客户区基线为 1280x960，窗口先隐藏创建，在入口配置完成后由宿主显示；不等待游戏资源加载。
---@class WindowModule
Window = {}

---设置主窗口 UTF-8 标题；不能包含 NUL。成功后可读取，平台失败抛错。
---@param title string
function Window.SetTitle(title) end

---返回当前主窗口标题；初始标题由宿主版本标识提供。
---@return string title
function Window.GetTitle() end

---请求改变窗口客户区尺寸；单位为窗口坐标，非 DPI 物理像素，亦不改变离屏 Canvas。
---宽高必须为 1..2147483647 的整数。实际生效时间、全屏尺寸约束由窗口系统决定。
---@param width integer
---@param height integer
function Window.SetSize(width, height) end

---返回本帧已确认客户区逻辑尺寸；与 Input.GetMousePosition 共用快照，不含边框/标题栏。
---首个输入帧前读取已应用启动尺寸；帧内请求 resize 不追改已发布快照，下一采集更新。
---窗口逻辑中心为 (0,0)，X 右正/Y 上正；GPU 像素尺寸不向 Lua 暴露。
---@return integer width
---@return integer height
function Window.GetSize() end

---请求无边框桌面全屏/恢复窗口模式；默认 false，不改变显示器分辨率。
---隐藏启动阶段请求在首次显示时同步；其他情形可能延迟生效，尺寸/位置恢复由 SDL 处理。
---@param enabled boolean
function Window.SetFullscreen(enabled) end

---读取窗口事件已经确认的全屏状态；不会把隐藏窗口的待应用请求报告成已生效。
---@return boolean fullscreen
function Window.IsFullscreen() end

---请求允许用户拖动边框调整尺寸，默认 true；不禁止脚本 SetSize。
---@param enabled boolean
function Window.SetResizable(enabled) end

---返回当前窗口可调整大小标志。
---@return boolean resizable
function Window.IsResizable() end

---配置 GPU 呈现模式；默认 true（VSync），false 请求 Immediate；不改变 Runtime 目标 FPS。
---模式不支持时抛错且保留已应用值。通常同步生效；本帧已获取交换链时延至安全边界应用。
---延迟请求应用前 Getter 仍读旧值；重复请求当前已应用模式会取消待应用变更。
---@param enabled boolean
function Window.SetVSync(enabled) end

---返回已生效的同步模式；待应用请求不会提前改变结果。
---@return boolean enabled
function Window.IsVSyncEnabled() end

---设置系统光标可见性；初始化沿用 SDL 光标状态，不控制 Input 鼠标启用开关。
---@param visible boolean
function Window.SetCursorVisible(visible) end

---返回 SDL 当前系统光标可见状态。
---@return boolean visible
function Window.IsCursorVisible() end

---读取主窗口是否拥有键盘输入焦点；失焦不会自动暂停游戏逻辑。
---@return boolean focused
function Window.IsFocused() end

---读取窗口事件已经确认的系统最小化状态；隐藏、失焦或交换链无图像不等于最小化。
---查询不泵事件、不获取/等待 GPU，也不暂停 Lua、音频或离屏工作；服务无效时抛错。
---@return boolean minimized
function Window.IsMinimized() end
