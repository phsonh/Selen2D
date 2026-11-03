---@meta

-- Selen2D v2.0.0 一号机基线声明；仅供编辑器，不应由游戏执行。
-- 核对来源：lua_bind/runtime/Runtime.cpp、core/runtime/Clock、core/runtime/Lua、engine/Engine.cpp。
-- 设计依据：devs/design/Solongo API 一号机/runtime/Runtime.md。
-- 所有函数严格检查实参数量和类型；不将字符串转换为数字。调用错误抛 Lua 错误。
-- Runtime 只调度整帧回调；不会自动更新、碰撞或绘制 Object，也不管理游戏暂停/Replay。
---@class RuntimeModule
Runtime = {}

---替换唯一的整帧回调。当前回调仍自然返回，替换从下一次调度生效；回调无实参，返回值被丢弃。
---每轮最多调用一次，不追赶卡顿欠帧；GPU 帧由宿主建立。默认未注册，未注册时宿主仍运行但不调用 Lua 帧函数。
---必须是同一 Lua VM 的函数（该 VM 的 coroutine 可以注册）；不能传 nil 清除。
---回调不得越过宿主 C 调用边界 yield；未捕获错误由宿主报告并进入退出流程。
---@param func fun()
function Runtime.SetFrameFunction(func) end

---替换正常退出回调；无实参、返回值丢弃，退出阶段最多调用一次（抛错或重新注册也不重试）。
---默认未注册。执行时 Lua、File、Audio、资源仍有效，但普通绘图没有活动帧；强杀/崩溃不保证调用。
---必须传同一 VM 的函数，不接受 nil；回调错误被报告后仍继续清理。
---@param func fun()
function Runtime.SetExitFunction(func) end

---请求退出，无返回值。立即设置请求，当前 Lua 调用继续执行，宿主在安全边界结束调度并执行退出回调。
---重复请求安全；不会直接销毁 VM，不自动中断当前 Lua 语句。
function Runtime.Quit() end

---立即修改目标主循环 FPS（默认 60），本轮余下等待使用新预算；超时不追赶历史欠帧。
---fps 必须有限且 0 < fps <= 1000，且对应纳秒帧间隔可表示为 int64；否则抛错并保持原值。
---这不改变游戏逻辑更新次数，也不同于 Window 的 VSync。
---@param fps number
function Runtime.SetTargetFPS(fps) end

---读取当前目标配置，不进行测量；初始化默认 60。
---@return number fps
function Runtime.GetTargetFPS() end

---立即读取已采集帧间隔的实际 FPS；尚无有效间隔时为 0。
---基线用最近至多 60 个 Runtime 帧开始间隔计算，不等于目标值；统计窗口为实现细节。
---@return number fps
function Runtime.GetCurrentFPS() end
