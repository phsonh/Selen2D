---@meta

---Selen2D Object V2 接口。声明文件只供编辑器使用，不应 require/dofile 执行。
---Lua 显式调用 Commit 提交生命周期，再按需要调用更新、渲染、碰撞、越界检查；Runtime 不代为提交或调度。
---数值参数和原生字段必须为有限数；参数个数、类型、跨宿主对象、服务关闭及回调异常均报 Lua 错误。
---Commit/UpdateAll/RenderAll/CheckCollision/CheckOutBound 禁止在任何 Object 回调内直接、间接或经协程重入。
---只读查询及 New/Delete 不受此批量重入禁令影响。回调错误停止当前普通批量调用，已有副作用不回滚。
---@class Object
Object = {}

---由 Object.Class 注册的类表。回调按继承链动态查找；未定义回调不执行，非函数回调报错。
---类表可附加自定义字段。base 只读；继承回调中宜使用定义类时捕获的父类引用。
---@class ObjectClass
---@field base? ObjectClass 只读的直接父类；根类为 nil。
---@field Init? fun(self: ObjectInstance, ...) 创建时同步执行一次，任意附加参数原样传入。
---@field Update? fun(self: ObjectInstance) 原生运动与 timer 增加之后调用。
---@field Render? fun(self: ObjectInstance) 自行提交 Graphic 绘制；Object 不绘制默认图像。
---@field Colli? fun(self: ObjectInstance, other: ObjectInstance) 一次命中对应一个对侧对象。
---@field Delete? fun(self: ObjectInstance) 显式 Commit 或退出收尾时最多执行一次；可读取自身全部字段，可写自定义字段，禁止写原生字段。

---原生 userdata；普通 Lua 字段优先于继承类字段，原生字段直接读写对象池。
---有效对象的属性写入立即生效。待激活对象可以读写，显式 Commit 激活后才参与批量调度。
---待删除/过期对象的读取和写入报错；仅其自身 Delete 回调享有读取和修改自定义字段的例外。
---group、timer 为 32 位整数；其余数值字段有限。a/b/rate 另要求非负，开关只接受 boolean。
---layer/navi/hide/omega/omiga 若使用，仅是自定义字段，没有内建行为；没有自动角速度、排序或级联删除。
---@class ObjectInstance
---@field class ObjectClass 只读所属类。
---@field x number 中心世界坐标，默认 0。
---@field y number 中心世界坐标，默认 0。
---@field rot number 角度（度），默认 0；仅旋转矩形碰撞使用，不自动增长。
---@field vx number 每逻辑步 X 速度，默认 0。
---@field vy number 每逻辑步 Y 速度，默认 0。
---@field ax number 每逻辑步 X 加速度，默认 0。
---@field ay number 每逻辑步 Y 加速度，默认 0。
---@field rate number 原生运动倍率，默认 1，范围 [0,+∞)；不缩放 timer 或回调频率。
---@field a number 默认 0；圆形参与半径 (a+b)/2，矩形为半宽。
---@field b number 默认 0；圆形参与半径 (a+b)/2，矩形为半高。
---@field group? integer 默认 nil（不属任何碰撞组）；整数范围 0..2147483647。
---@field rect boolean 默认 false 为圆形；true 为旋转矩形；零尺寸允许，不支持椭圆。
---@field colli boolean 默认 true，决定是否作为碰撞候选，与其他开关独立。
---@field bound boolean 默认 true，决定是否接受中心越界删除。
---@field update boolean 默认 true；false 同时停止原生运动、timer 和 Update 回调。
---@field render boolean 默认 true，控制 Render 回调。
---@field timer integer 默认 0，可写 -2147483648..2147483647；表示 UpdateAll 次数，并非帧数或毫秒。

---立即注册类并返回可扩展类表，不创建对象。parent 可省略或 nil；普通 table 不能充当父类。
---@param parent? ObjectClass 本服务已经注册的父类。
---@return ObjectClass class
function Object.Class(parent) end

---立即分配句柄并同步调用 class.Init(obj, ...)，无 Init 时只创建；成功后默认字段可直接使用。
---新对象立即计入 count 且 IsValid 为 true，直到之后显式 Commit 才激活；无需等待宿主帧。
---Init 可以创建其他对象或 Delete(self)。Init 抛错原样保留已建立的对象、字段与请求，不隐式 Delete、回收或回滚。
---Init 抛错时 New 没有成功返回；可用 Init 暴露的 self 或 GetAll 找到仍有效者，调用方自行处理。
---class 必须为本服务注册类；参数错误或池满在建立对象前报错且不调用 Init。池满不自动扩容或 Commit。
---@param class ObjectClass
---@param ... any 传给 Init 的附加参数。
---@return ObjectInstance object
function Object.New(class, ...) end

---首次请求后立即无效且 count 减一，后续调度跳过；槽位仍占用，之后显式 Commit 才调用 Delete 并回收。
---重复删除或删除本服务的过期句柄返回 false；非 Object 或其他服务句柄报错。
---按第一次请求的 FIFO 顺序排队，重复请求不改变次序。Delete 回调新增的请求留到下一次 Commit。
---退出阶段先执行 ExitFunction，再动态清空删除队列，回调新增删除追加队尾；未请求删除的存活对象只释放存储。
---@param object ObjectInstance
---@return boolean changed 首次成功标记为 true，已待删除或已过期为 false。
function Object.Delete(object) end

---立即查询有效性；待激活对象为 true，待删除/过期/其他服务对象以及任意非 Object 值为 false。
---旧句柄不会因池槽位复用重新有效；服务关闭或参数数目不符仍会报错。
---@param value any 必须显式传一个值，可为 nil。
---@return boolean valid
function Object.IsValid(value) end

---立即返回宿主初始化的实际对象池容量，在此运行实例内固定；无参数，只读且无隐式 Commit。
---容量是最大同时占用槽位数，不是 count、空闲数、reserve 值或字节数；零容量返回 0。
---待删除对象及其 Delete 回调尚未返回时仍占槽位，因此 capacity-count 不等于可立即 New 的数量。
---@return integer capacity 非负整数。
function Object.GetPoolCapacity() end

---立即返回 IsValid 为 true 的对象数，包含待激活对象，排除待删除/已回收对象。
---包含正在 Init 和 Init 失败后仍有效者；不受 group、rate、timer 或调度开关影响。无参数，无隐式 Commit。
---New 建立时立即增加，首次 Delete 立即减少，激活与后续回收不重复增减；可在 Object 回调中查询。
---@return integer count 非负整数。
function Object.GetObjectCount() end

---立即捕获所有有效对象的身份，按句柄建立顺序返回单个闭包，可直接用于 for obj in Object.GetAll() do。
---包括 group=nil、Init 中、Init 失败后仍有效和待激活对象；不受四种调度开关影响。无参数，无隐式 Commit。
---每次调用闭包返回一个原对象；产出前重查代次与有效性，不重查 group。捕获后新建者不追加，已删除者跳过。
---快照不保留槽位，Commit 后槽位复用不会变成新对象；循环中在合法位置可自行 Commit，嵌套迭代互不干扰。
---耗尽后始终返回一个 nil；服务销毁后未耗尽迭代器报状态错误，已耗尽迭代器仍返回 nil。
---@return fun(): ObjectInstance? iterator
function Object.GetAll() end

---立即捕获当时有效且属于 group 的对象身份，按建立顺序返回单个闭包；含 Init 中及待激活者，不受调度开关影响。
---每次产出前重查代次、有效性和当前 group：删除/离组者跳过，原候选离组后在产出前回来仍可产出。
---捕获后新建或从其他组加入者不追加；槽位复用不替换快照身份。迭代、关闭、单 nil 耗尽规则同 GetAll。
---无隐式 Commit、字段修改或回调。nil 不是全池/未分组查询；需要未分组者可在 GetAll 中自行过滤。
---@param group integer 0..2147483647，必须显式提供且不可为 nil。
---@return fun(): ObjectInstance? iterator
function Object.GetGroup(group) end

---用当前 x/y/rot/rect/a/b 执行纯几何查询，支持圆/OBB，相切算命中；同一有效对象查询自身返回 false。
---允许有效待激活者，不受 group 或 colli/update/render/bound 开关影响，不调用 Colli、不修改对象或隐式 Commit。
---两个参数必须是本服务有效对象；待删除、已回收、非 Object 或其他服务对象报错，不将错误当作不相交。
---@param first ObjectInstance
---@param second ObjectInstance
---@return boolean colliding
function Object.IsColliding(first, second) end

---显式提交生命周期，无参数，正常返回零个值。可在入口、普通 Lua 函数和 FrameFunction 中同帧调用多次。
---进入时同时冻结删除请求和待激活身份两个快照；先按首次请求 FIFO 删除并回收，再按句柄建立顺序激活。
---Delete 回调新建/删除的请求均不加入本次快照；若回调删除本次待激活对象，该对象本次跳过激活，下次才回收。
---Delete 抛错仍回收当前对象，立即中断本次 Commit 且不开始激活；已完成者不重跑，旧剩余请求排在新增请求之前。
---Lua 捕获错误后可再次 Commit，按新快照继续；不回滚字段、New/Delete 或其他副作用，不设置隐藏退出标志。
---只处理 CPU 生命周期，不推进运动、timer、Time、输入或 GPU；所有 Object 回调及原生批量调度中重入均立即报错。
---宿主开始退出收尾后禁止 Commit（包括 ExitFunction）；仅 Runtime.Quit 请求尚未返回当前 Lua 调用时不算此边界。
---退出收尾先 ExitFunction 后动态清空删除队列，错误记录后继续；新请求追加尾部，无隐式激活或未请求对象的 Delete。
function Object.Commit() end

---执行一次逻辑步；按稳定激活次序处理活动且 update=true 的对象。
---依次 vx+=ax*rate、vy+=ay*rate，再 x+=vx*rate、y+=vy*rate，timer+=1，最后调用 Update。
---rate=0 仍增加 timer 并调用 Update；首次回调读到 timer=1。重复调用就重复推进，不按真实时间补步。
---timer 达 2147483647 或积分结果非有限时抛错；该对象的运动和 timer 不部分提交。
---入口固定活动对象身份，调用前重查有效性和 update；错误停止本次，下一次调用从新快照首项开始，不续跑旧游标。
---无参数，无返回值，不隐式 Commit；不得在任何 Object 回调或原生批量调度中重入。
function Object.UpdateAll() end

---按稳定激活次序调用活动、有效且 render=true 对象的 Render；同帧重复调用会重复渲染。
---不按自定义 layer 排序，不切换 Canvas，不自动开始/结束图形帧；Graphic 绘制仍需合法 FrameFunction 阶段。
---入口固定活动对象身份，调用前重查有效性和 render；错误停止本次，下次重新从新快照开始。
---无参数，无返回值，不隐式 Commit；不得在任何 Object 回调或原生批量调度中重入。
function Object.RenderAll() end

---检查活动、有效、colli=true 且属于指定组的对象；group=nil 不参与。
---异组按 A 为外层、B 为内层的稳定激活顺序，仅向 A 分发 Colli；同组按 i<j 字典序，每对先较早激活侧再另一侧。
---同组跳过自身，每对只计算一次几何；第二侧调用前复核双方资格，不重新计算该对几何。
---每次调用固定入口组成员集合，回调中新加入成员不追加；每次分发前重查有效性、组与开关。
---第一侧删除对侧会取消第二侧回调；当前位置/尺寸修改影响后续对象对。相切算命中，不自动删除或反弹。
---只支持圆-圆、圆-旋转矩形、旋转矩形-旋转矩形；每次调用重新检测，无跨调用去重。
---无返回值，不隐式 Commit；非法组号、任何 Object 回调/原生批量调度中重入均报错；回调错误停止本次，下次从新快照开始。
---@param groupA integer 0..2147483647，不接受 nil。
---@param groupB integer 0..2147483647，不接受 nil。
function Object.CheckCollision(groupA, groupB) end

---立即替换世界边界；四值均有限且 left<=right、bottom<=top，允许零范围。
---无默认边界；成功设置只改变后续越界检查，不立即删除对象。非法参数不改变旧配置，无返回值。
---@param left number
---@param right number
---@param bottom number
---@param top number
function Object.SetBound(left, right, bottom, top) end

---只读返回最近一次成功设置的边界，不执行越界检查。未调用 SetBound 前立即报错，即使池为空。
---@return number left
---@return number right
---@return number bottom
---@return number top
function Object.GetBound() end

---按对象中心 x/y 检查活动且 bound=true 的对象，严格小于/大于边界时调用统一删除流程。
---边界上的点不越界；不考虑 a/b/rot，bound=false 跳过，不执行额外越界回调。
---未配置 SetBound 时立即报错；禁止任何 Object 回调内调用或原生批量调度中重入。无参数，无返回值，不隐式 Commit。
function Object.CheckOutBound() end
