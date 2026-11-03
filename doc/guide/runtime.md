# 帧调度、时间与错误

入口脚本执行一次。`Runtime.SetFrameFunction` 注册整个游戏帧；Lua 自行决定何时调用
`Object.Commit`、更新、碰撞和绘制。宿主不代办对象生命周期，也不会在卡顿后补逻辑帧。

```lua
local Particle = Object.Class()
function Particle:Init() self.vx = 1 end
local particle = Object.New(Particle)
Runtime.SetFrameFunction(function()
    Object.Commit()
    Object.UpdateAll()
    -- 绘制由游戏自行提交；没有 Render 回调时对象只参与模拟。
end)
Runtime.SetExitFunction(function()
    Object.Delete(particle)
    -- 关闭阶段不能 Commit；宿主随后收尾已请求的 Delete，其他对象只释放。
end)
```

`Runtime.GetArguments()` 每次返回独立字符串数组，包含宿主接收到的全部参数（不含程序名），
保留中文、空项、重复项和单个参数中的空格。它不改入口或文件授权。

`Debug.Error(message)` 记录后立即抛错。`pcall`、`xpcall`、协程可以捕获，捕获后没有隐含退出请求。
未捕获的入口／帧错误和正常退出都使用同一条关闭流程：已注册 ExitFunction 至多一次，
然后收尾已请求的 Object.Delete，最后按依赖释放资源。清理错误追加诊断，不覆盖最初原因。

```lua
local ok, message = pcall(Debug.Error, '配置不完整')
assert(not ok)
-- 可以继续执行；主动退出另用 Runtime.Quit()。
local seconds = Time.FramesToSeconds(180, 60)
assert(seconds == 3)
assert(Time.SecondsToFrames(-1.5, 1) == -2)
```

Time 两个换算函数必须显式提供有限正数 fps，允许负的帧数／秒数；半帧舍入远离零。
不读取 Runtime 目标或实际 FPS。乘除产生非有限结果时报错；整数帧结果限制在
`[-(2^53-1), 2^53-1]`，不作窄整数转换或环绕。可表示结果使用普通 double 运算。

本版帧率实现细节：目标默认 60，允许 `(0,1000]` 且纳秒预算可用 int64 表示。
未注册帧函数时仅维护宿主空帧；计数、Delta 和实际 FPS 在首次 Lua 帧前为 0。
第一帧计数为 1、Delta 为 0；之后 Delta 是两次实际回调开始的时间差，帧内保持不变。
实际 FPS 使用最近最多 60 个回调间隔，包含卡顿。每轮只按自身实际开始时间限速，没有历史欠帧。
