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

帧率规则：目标默认 60，允许 `(0,1000]` 且纳秒预算可用 int64 表示。
未注册帧函数时仅维护宿主空帧；计数、Delta 和实际 FPS 在首次 Lua 帧前为 0。
第一帧计数为 1、Delta 为 0；之后 Delta 是两次实际回调开始的时间差，帧内保持不变。
实际 FPS 使用最近最多 60 个回调间隔，包含卡顿；它是 Lua 回调频率，不是显示器实际显示的 FPS。

### 软件限帧与节拍误差

2.3.2 按当前帧实际预算精确限速，**任何迟到均不跨帧追赶**。宿主先记录本帧实际开始时间，
执行事件、资源与上传维护、GPU 帧槽准备、Lua 和渲染提交，再等待本帧预算的剩余部分。
已达到或超过预算就不等待；下一帧以自己的实际开始时间重新计算。睡眠超时、拖动、恢复和长卡顿
都不会缩短后续帧的预算，不补跑历史逻辑帧。2.3.1 的有限相位补偿已撤销。

长等待分段处理事件，不会在分段之间重新起算预算。接近本帧截止时使用 `SDL_DelayPrecise`，
最后最多 1 ms 使用短暂忙等，避免对亚毫秒剩余时间再次进入操作系统睡眠。
这会增加 CPU 占用，尤其是接近 1000 FPS 的轻负载；SDL 精确等待本身也可能包含忙等。
这些等待都只服务当前帧，不维护跨帧迟误或欠账。输入在等待和准备期间持续采集，并在下一次回调前发布。
等待中请求退出会进入原有关闭流程；录制槽等待或准备阶段退出时仍安全取消已取得的槽。

改变 `SetTargetFPS` 时，本帧余下等待立即按当前目标重算；重复设置同一目标不改变本帧起点。
首帧不偿还启动耗时；未注册回调时宿主仍按目标运行，但不增加 Lua 帧计数。
FPS、Delta 仍在准备完成后的真实 Lua 回调开始时统计，不提前打点、不截断长间隔。
宿主帧起点与回调起点不同：准备耗时突然变长后又恢复，可能产生较短的真实回调间隔，
这不代表限帧器缩短了后续宿主帧预算，也不能据此伪造或修正 Delta。

软件限速与 `Window.SetVSync` 独立：引擎不替游戏改变 VSync，也不会因 VSync 开启就取消软件限速。
实际可达频率仍受工作量、交换链、系统等待精度和调度影响；目标 `(0,1000]` 不是实时保证，0 也不表示不限帧。

## Object 极坐标运动辅助

`Object.SetVelocity(object, speed, angle)` 和 `Object.SetAcceleration(object, acceleration, angle)`
把极坐标转换后设置 `vx/vy` 或 `ax/ay`；对应的 `AddVelocity`、`AddAcceleration` 将转换后的分量加到当前值。
四个函数严格接收三个参数，成功只返回 `true`；错误抛出，不返回 `false, err`。幅值和角度必须为有限 number，
不接受数字字符串、NaN 或无穷大。角度使用度，0 沿 +X、90 沿 +Y；允许负幅值和任意有限角度。

`Object.GetVelocity(object)`、`Object.GetAcceleration(object)` 严格接收一个对象，返回当前分量计算出的
非负模长与 `atan2` 角度（`[-180,180]` 度）。零向量固定返回 `0,0`；模长溢出时报错。
直接修改 `vx/vy/ax/ay` 后立即反映，没有另外缓存的幅值或角度，也没有对象方法别名。

```lua
local bullet = Object.New(Object.Class())
Object.SetVelocity(bullet, 3, 90)       -- 尚未 Commit 也可设置运动。
Object.AddVelocity(bullet, 1, 0)
Object.SetAcceleration(bullet, 0.02, -90)
local speed, angle = Object.GetVelocity(bullet)
```

计算和校验全部成功后才一起写入两个分量，失败不会只更新其中一个。Add 只要求最终两个分量有限，
不额外要求它们的合成模长可表示；因此一次 Add 成功后，Get 仍可能因极大模长而报错。
这些调用不修改位置、`rot`、`rate`、`timer` 或另一组运动分量，不执行 Update、Commit 或回调。

有效的待激活对象可以使用全部六个函数。待删除、过期或其他服务的对象报错；仅在对象自身的 Delete 回调内，
Get 可按既有只读例外读取当前运动，Set/Add 仍禁止写入。原生积分继续在 `Object.UpdateAll()` 中执行。

## 碰撞分发与性能边界

`Object.CheckCollision(a, b)` 保留入口的活动组成员及其顺序，也包含当时 `colli=false` 的成员。
异组按源对象、目标对象顺序检查；同组按 `i < j`，命中后先调用较早对象，再检查双方资格决定是否反向调用。
同一对只计算一次几何，后续对象对读取最新状态。回调新建或从其他组加入的对象不会追加到本次成员集合。

引擎可先用保守包围范围排除明显分离的对象。第一次调用碰撞回调后，本次调用剩余配对不再使用这些范围，
因此回调移动第三个对象、启用原本关闭的成员或直接修改原生字段都不会被旧空间信息漏掉。
只有首个有效配对未命中后才准备范围；立即命中的调用直接保留原路径，避免无用准备。
需要时每次重新准备范围；仅复用容量，不跨帧缓存几何。极端数值仍使用原精确检测及错误规则。

这是精确检测次数优化，不是空间网格。配对枚举仍为异组 O(A×B)、同组 O(N²)；密集命中或很早发生回调时收益有限，
也可能支付少量准备成本。可选缓冲最多 512 KiB，每个对象池独立，关闭时释放；小规模调用和缓冲不可用时直接使用原路径。
