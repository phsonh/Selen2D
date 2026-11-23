---@meta

-- Selen2D Window 声明（17 个公开函数）；仅供编辑器，不应执行。
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

---返回当前游戏窗口关联显示器的桌面模式像素宽高；不是窗口客户区、工作区或全部显示器合并尺寸。
---按 SDL 确定窗口关联显示器；扩展屏跟随窗口，复制屏遵循系统提供的桌面模式，不查询面板最大支持分辨率。
---严格零参数，返回两个正整数；按 SDL desktop mode 的 pixel_density 换算为像素并四舍五入，不乘系统 UI 缩放倍率。
---每次读取当前已知显示状态，不缓存启动值、不泵事件或等待切换完成；移屏/桌面变化在 SDL 更新后反映。
---入口执行前隐藏窗口已创建，因此可在入口、帧内和窗口仍有效的退出回调中查询；查询不显示或调整窗口、不切换全屏。
---调用线程、窗口生命周期、平台查询或返回尺寸无效时抛错；Window.SetFullscreen 的原有行为保持不变。
---@return integer width 桌面模式像素宽度。
---@return integer height 桌面模式像素高度。
function Window.GetScreenSize() end

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

---请求还原最小化/最大化窗口；无参数、无返回值，平台请求失败抛错。
---返回只表示请求已受理；不额外同步等待完成、不泵事件、不提前改变 IsMinimized/IsMaximized。
---不额外调用 SDL_SyncWindow；平台调用仍可能耗时，SDL 同步窗口环境选项也可能使底层等待。
---后续帧需确认非全屏且两者为 false，再 SetSize；保留 GetSize 与输入的本帧尺寸快照。
---最小化窗口可能先恢复到原最大化状态；不能假定一次请求必然回到普通窗口。
---隐藏启动时不显示窗口，只取消待应用的最小化/最大化请求，不取消待应用全屏。
---全屏时不直接还原、不隐式退出全屏；SDL 可能改变以后退出全屏的窗口化状态。
---不额外请求聚焦/显示；实际还原产生的激活/显示由窗口系统处理。普通窗口重复调用不调整尺寸。
---须在窗口有效的 SDL 主线程调用；服务已关闭或线程错误抛错。
function Window.Restore() end

---返回窗口事件已经确认的最大化状态；与 IsMinimized/IsFullscreen 一样，不把请求当作完成。
---最小化或还原事件清除此状态；全屏不是最大化的同义词，不由全屏尺寸推断。
---无参数，返回一个 boolean；查询不泵事件、不等待 GPU，服务/线程无效时抛错。
---@return boolean maximized
function Window.IsMaximized() end
