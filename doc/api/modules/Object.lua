---@meta

---Selen2D 2.0.0 一号机对象接口。声明文件只供编辑器使用，不应作为脚本执行。
---依据基线 42585671780bca75692550685e49dcb824e2d36e 的 Object Lua Binding 与一号机规范核对。
---Runtime 仅在实际帧开始、FrameFunction 前维护激活/删除队列；游戏必须自行调用批量更新、渲染、碰撞、越界检查。
---数值参数和原生字段必须为有限数；参数个数、类型、跨宿主对象、服务关闭及回调异常均报 Lua 错误。
---所有批量调度禁止重入，也禁止在 Init 内调用；回调发生错误会停止当前调度，已完成的对象更新不回滚。
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
---@field Delete? fun(self: ObjectInstance) 下一实际帧销毁阶段执行；可读取自身全部字段，可写自定义字段，但禁止写原生字段。

---原生 userdata；普通 Lua 字段优先于继承类字段，原生字段直接读写对象池。
---有效对象的属性写入立即生效。待激活对象可以读写，但要到下一实际帧才参与批量调度。
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
---新对象 IsValid 为 true，但下一 Runtime 实际帧开始才激活；当帧重复批量调度不会提前激活。
---Init 可以创建其他对象或 Delete(self)。Init 抛错会回收本对象且不执行 Delete；已创建的其他对象不自动撤销。
---class 必须是本服务注册的类；对象池已满、非法回调或 Init 异常报错。
---@param class ObjectClass
---@param ... any 传给 Init 的附加参数。
---@return ObjectInstance object
function Object.New(class, ...) end

---请求删除后立即变为无效，后续调度跳过；下一实际帧执行一次 Delete 回调并回收槽位。
---重复删除或删除本服务的过期句柄返回 false；非 Object 或其他服务句柄报错。
---帧边界按入口队列快照处理；Delete 回调中新建/删除的请求留到下一个帧边界。
---@param object ObjectInstance
---@return boolean changed 首次成功标记为 true，已待删除或已过期为 false。
function Object.Delete(object) end

---立即查询有效性；待激活对象为 true，待删除/过期/其他服务对象以及任意非 Object 值为 false。
---旧句柄不会因池槽位复用重新有效；服务关闭或参数数目不符仍会报错。
---@param value any 必须显式传一个值，可为 nil。
---@return boolean valid
function Object.IsValid(value) end

---立即返回 IsValid 为 true 的对象数，包含待激活对象，排除待删除/已回收对象。
---不受 update/render/colli/bound 开关影响，无需等待帧边界。无参数。
---@return integer count 非负整数。
function Object.GetObjectCount() end

---执行一次逻辑步；按稳定激活次序处理活动且 update=true 的对象。
---依次 vx+=ax*rate、vy+=ay*rate，再 x+=vx*rate、y+=vy*rate，timer+=1，最后调用 Update。
---rate=0 仍增加 timer 并调用 Update；首次回调读到 timer=1。重复调用就重复推进，不按真实时间补步。
---timer 达 2147483647 或积分结果非有限时抛错；该对象的运动和 timer 不部分提交。
---无参数，无返回值。不得从 Init 或任何 Object 批量调度回调中调用。
function Object.UpdateAll() end

---按稳定激活次序调用活动、有效且 render=true 对象的 Render；同帧重复调用会重复渲染。
---不按自定义 layer 排序，不切换 Canvas，不自动开始/结束图形帧；Graphic 绘制仍需合法 FrameFunction 阶段。
---无参数，无返回值；不得从 Init 或 Object 批量调度中重入。
function Object.RenderAll() end

---检查活动、有效、colli=true 且属于指定组的对象；group=nil 不参与。
---异组只向 groupA 侧分发 Colli；同组跳过自身、每对只计算一次几何，并按稳定顺序给双方回调机会。
---每次调用固定入口组成员集合，回调中新加入成员不追加；每次分发前重查有效性、组与开关。
---第一侧删除对侧会取消第二侧回调；当前位置/尺寸修改影响后续对象对。相切算命中，不自动删除或反弹。
---只支持圆-圆、圆-旋转矩形、旋转矩形-旋转矩形；每次调用重新检测，无跨调用去重。
---无返回值；非法组号、批量重入或回调异常报错。
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
---未配置 SetBound 时立即报错；禁止 Init 内调用或批量重入。无参数，无返回值。
function Object.CheckOutBound() end
