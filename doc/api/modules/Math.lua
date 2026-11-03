---@meta

-- Selen2D v2.0.0 一号机基线声明；仅供编辑器，不应执行。
-- 来源：lua_bind/modules/Math、core/modules/Math；设计依据：一号机/modules/Math.md。
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

---一条进程全局随机序列，默认种子 0；Xorshift128 11/8/19、SplitMix64 展开四个低 32 位状态字。
---相同种子与调用顺序/参数重现相同序列；所有随机 API 共用状态，不与 math.random 共用。
---仅适用于游戏模拟，非密码学随机源。改变调用顺序会改变 Replay；无实例构造器或状态序列化接口。
---@class MathRandom
Math.Random = {}

---立即重置全部 RNG 状态，无返回值；非法调用不推进序列。
---@param seed integer 必填，范围 [0,4294967295]，拒绝截断/取模/非有限数。
function Math.Random.Seed(seed) end

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
