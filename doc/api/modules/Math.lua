---@meta

-- Selen2D Math 声明；仅供编辑器，不应执行。
-- 来源：lua_bind/modules/Math、core/modules/Math。
-- 运算同步完成；参数数量、类型、范围不正确均抛 Lua 错误，不接受数字字符串。
-- 三角函数为角度制，不改变 Lua 标准 math 的弧度制；不保证跨平台三角函数逐位一致。
---@class MathModule
Math = {}

---计算角度正弦；参数必须有限，没有默认值。
---@param degrees number 角度。
---@return number value
function Math.sin(degrees) end

---计算角度余弦；参数必须有限，没有默认值。
---@param degrees number 角度。
---@return number value
function Math.cos(degrees) end

---计算角度正切；参数必须有限。90+180k 度的浮点近似结果不固定，不保证返回无穷。
---@param degrees number 角度。
---@return number value
function Math.tan(degrees) end

---计算反正弦，返回角度；输入必须有限且在 [-1,1]。
---@param value number
---@return number degrees 通常位于 [-90,90]。
function Math.asin(value) end

---计算反余弦，返回角度；输入必须有限且在 [-1,1]。
---@param value number
---@return number degrees 位于 [0,180]。
function Math.acos(value) end

---计算向量 (x,y) 的方向角；两个参数均必须有限。(0,0) 特定返回 0。
---@param y number
---@param x number
---@return number degrees 位于 [-180,180]，轴向遵循标准 atan2。
function Math.atan2(y, x) end

---独立 Xorshift128 11/8/19 生成器，SplitMix64 展开四个低 32 位状态字；原序列及消费规则固定。
---同一 VM 协程共享当前选择；初始化选中 seed=0 的生成器，当前选择持有强引用。
---模块调用操作当前对象，对象方法只操作 self；Create/Clone/切换/状态查询恢复均不抽样。
---Math 和 Math.Random 是可扩展普通表，不与 Lua math.random 共用状态。仅适用于游戏模拟，非密码学随机源。
---@class MathRandom
Math.Random = {}

---@class MathRandomState
---@field algorithm 'xorshift128-11-8-19'
---@field seed integer 最近 Seed/Create/SetState 的 uint32 种子记录。
---@field x integer uint32 当前寄存器状态。
---@field y integer uint32 当前寄存器状态。
---@field z integer uint32 当前寄存器状态。
---@field w integer uint32 当前寄存器状态；x/y/z/w 不得全零。

---@class MathRandomGenerator
local Generator = {}

---创建独立对象，不改变当前选择，不消费其他流；无 Destroy，引用和 Lua GC 管理生命周期。
---@param seed? integer 省略或 nil 为 0；否则须为 uint32 整数。
---@return MathRandomGenerator rng
function Math.Random.Create(seed) end

---选择存活生成器，成功无返回值；不接受 nil，非法调用保持旧选择。
---@param rng MathRandomGenerator
function Math.Random.SetGenerator(rng) end

---返回相同的当前 userdata，不创建副本。
---@return MathRandomGenerator rng
function Math.Random.GetGenerator() end

---立即重置全部 RNG 状态，无返回值；非法调用不推进序列。
---@param seed integer 必填，范围 [0,4294967295]，拒绝截断/取模/非有限数。
function Math.Random.Seed(seed) end

---返回当前生成器最近记录的 uint32 种子，不代表当前位置。
---@return integer seed
function Math.Random.GetSeed() end

---在闭区间 [first,last] 无偏抽取整数；端点须为 |n|<=2^53-1 的整数且 first<=last。
---区间元素数不得超过 2^32；用拒绝采样，可能消耗多次原始输出。
---first==last 直接返回该值且不消耗状态；非法调用抛错且不推进状态。
---@param first integer
---@param last integer
---@return integer value
function Math.Random.Int(first, last) end

---以一个 uint32/2^32 映射到 [first,last)；端点必须有限且 first<=last。
---first==last 返回该值且不消耗状态；其他有效调用消耗一次输出，舍入也不会返回 last。
---非法调用不推进状态；采用 double 运算，但不作所有平台浮点逐位一致的额外承诺。
---@param first number
---@param last number
---@return number value
function Math.Random.Float(first, last) end

---等概率返回 -1 或 1；消耗一次原始输出，按最低位确定符号，没有参数或默认值。
---@return -1|1 sign
function Math.Random.Sign() end

---返回恰好六字段的独立普通表；修改副本不影响生成器，不消耗输出。
---@return MathRandomState state
function Math.Random.GetState() end

---仅读自身六字段；algorithm 精确匹配，五个数须为 uint32，四状态字不得全零。
---缺键、多键、错误类型/范围/算法均报错且原状态不变；复制当前状态，不按 seed 重新展开。
---成功无返回值，不保留 table 引用，不执行 __index/__pairs 等元方法。
---@param state MathRandomState
function Math.Random.SetState(state) end

---复制当前四字状态和种子记录至独立对象，不改变当前选择或推进任何随机流。
---@return MathRandomGenerator rng
function Math.Random.Clone() end

---对象形式规则与模块 Seed 相同，不改变当前选择。
---@param seed integer
function Generator:Seed(seed) end

---@return integer seed
function Generator:GetSeed() end

---闭区间无偏整数；参数与消费规则同 Math.Random.Int。
---@param first integer
---@param last integer
---@return integer value
function Generator:Int(first, last) end

---半开区间 double；参数与消费规则同 Math.Random.Float。
---@param first number
---@param last number
---@return number value
function Generator:Float(first, last) end

---@return -1|1 sign
function Generator:Sign() end

---@return MathRandomState state
function Generator:GetState() end

---原子复制六个 raw 字段；无返回值、无抽样，不改变默认选择。
---@param state MathRandomState
function Generator:SetState(state) end

---@return MathRandomGenerator rng
function Generator:Clone() end
