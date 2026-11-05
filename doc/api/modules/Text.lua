---@meta

-- Selen2D Text 声明；仅供编辑器，不应执行。
-- 来源：lua_bind/modules/Text/Text.cpp、core/modules/Text/Text.cpp。
-- 三个函数同步执行纯内存转换，不进行文件 I/O，不负责字体绘制；参数数量/类型错误抛 Lua 错误。
-- 输入开头允许单个 UTF-8 BOM；输出零统一为 0；非零 token 下溢到零报错，允许可表示的 subnormal。
-- 输入/输出各最多 8 MiB，最多 262144 个值节点（含容器），数字 token 最多 1024 字节。
-- 根深度为 0，允许到深度 128；深度/容量失败抛错，不返回部分结果。
---@alias TextTimeFormat 'mm:ss'|'hh:mm:ss'|'dd:hh:mm:ss'|'mm:ss.ms'
---@class TextModule
Text = {}

---解析 UTF-8 JSON/JSONC，根必须为对象或数组；允许 //、块注释及末尾逗号。
---对象为字符串键表，数组从 1 索引；null 转 nil（对象字段消失、数组可能空洞或丢失尾部长度）。
---非法 UTF-8（含注释）、未转义 NUL、语法、标量根、超限或 Lua 栈容量不足均抛错。
---同一对象的完整解码键必须唯一，含转义/NUL；前值 null 不解除占用，不做大小写或 NFC 归一化。
---整数仅允许 ±9007199254740991；原始十进制 token 含小数/指数但值为整数也先精确校验。
---转换后的非有限数或范围外整数仍拒绝；普通非整数按 Lua number 精度，不保留原十进制拼写。
---需要逐位保留的超界编号须用字符串；不提供 null 占位类型，不保证 null/空数组无损往返。
---@param jsonString string
---@return table value
function Text.Json2Table(jsonString) end

---同步序列化为紧凑标准 UTF-8 JSON；无缩进选项，字段顺序不作为接口保证。
---字符串键表为对象，非空连续 1..n 整数键表为数组，空表固定为 {}。
---只接受有限数字、布尔、合法 UTF-8 字符串及嵌套表；整数同样限制在 ±9007199254740991。
---控制字符和 NUL 转义输出；混合/稀疏/非法键、循环、非 JSON 类型、超限或非法 UTF-8 抛错。
---使用原始 table 字段，忽略元表 __index/__pairs；共享子表允许，循环不允许。
---错误附带可用的字段/数组路径，不调用用户序列化元方法，不修改输入表。
---@param luaTable table
---@return string json
function Text.Table2Json(luaTable) end

---格式化持续秒数；seconds 必须有限且非负，未知格式或错误类型抛错。
---无小数格式截去小数；mm:ss.ms 的 .ms 是两位百分之一秒，四舍五入并进位（59.999 -> 01:00.00）。
---最高单位累计且至少两位，不截断：mm 可超 59，hh 可超 23，dd 可超 99。
---较低单位固定两位；dd 格式的 hh 为 00..23，其余分钟/秒为 00..59。
---@param seconds number
---@param format? TextTimeFormat 省略或 nil 默认为 "mm:ss"。
---@return string formatted
function Text.FormatTime(seconds, format) end
