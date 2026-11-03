---@meta

-- Selen2D 二号机已实现声明；仅供编辑器，不应执行。
-- 来源：lua_bind/modules/Time/Time.cpp、core/modules/Time、core/runtime/Clock。
-- 设计依据：二号机 modules/Time.md。
-- 查询立即返回，不改变时钟。参数数量/类型不符、非法数值或转换溢出均抛 Lua 错误。
---@class TimeDate
---@field year integer 公历年。
---@field month integer 1..12。
---@field day integer 月内日期。
---@field hour integer 0..23。
---@field minute integer 0..59。
---@field second integer 本地日历秒字段。

---@class TimeModule
Time = {}

---返回当前系统本地时区日历时间的新表；系统调时会跳变，不能用来测量稳定耗时。
---系统无法读取本地日历时间时抛错；不包含时区字段。
---@return TimeDate date
function Time.GetDate() end

---从本次宿主初始化单调时钟基准开始的真实毫秒数，可含小数。
---游戏逻辑暂停、窗口拖动和系统日历修改均不会重置；包含真实等待时间。
---@return number milliseconds
function Time.GetElapsedMilliseconds() end

---已经开始调用的整帧回调次数。入口阶段为 0，第一帧回调中为 1，在每次回调前递增。
---Lua 自己额外执行 Update 不增加计数；不会补计错过帧。以 Lua number 返回，极大计数受双精度限制。
---@return integer frames
function Time.GetFrameCount() end

---最近两次 Runtime 帧开始之间的真实毫秒间隔；入口和第一帧为 0。
---窗口拖动恢复后可能很大，不截断、不等于回调自身耗时，不按目标 FPS 伪造。
---@return number milliseconds
function Time.GetDeltaMilliseconds() end

---返回 frames / fps，不取整；立即执行纯数值换算。
---frames 必须有限，允许负数和小数；fps 必须有限且大于 0。结果非有限时抛错。
---@param frames number
---@param fps number 必填；不读取 Runtime 目标或实际 FPS。
---@return number seconds
function Time.FramesToSeconds(frames, fps) end

---将 seconds * fps 舍入为最近整数帧，恰为半帧时远离零；立即执行，不修改任何计数。
---seconds 必须有限，允许负数；fps 必须有限且大于 0。结果绝对值超过 2^53-1 或非有限时抛错。
---@param seconds number
---@param fps number 必填；不读取 Runtime 目标或实际 FPS。
---@return integer frames
function Time.SecondsToFrames(seconds, fps) end
