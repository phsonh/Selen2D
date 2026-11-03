---@meta

---Selen2D 2.0.0 一号机图形接口；本文件只供编辑器，不应作为脚本运行。
---已按基线 42585671780bca75692550685e49dcb824e2d36e 的 Graphic/Mesh/ShaderParams 绑定、原生实现和一号机规范核对。
---模块为顶层 Graphic，独立于 Object；创建资源不自动显示，绘制不修改 Object 或共享 Image 的元数据。
---SetTargetCanvas/GetTargetCanvas/Clear/Render* 仅能在 Runtime 的实际 FrameFunction 绘制阶段调用，初始化和退出回调不能绘制。
---2D 在有序屏障之间按有限 layer 稳定排序，同层保留提交顺序；3D 完全按提交顺序。透明命令不按纹理重排。
---绘制参数和状态在调用时取快照，源 Canvas 像素按执行依赖读取；已接受命令保活所需资源到 GPU 完成。
---所有数值必须有限，颜色须为 0..4294967295 整数 ARGB；图形阶段、类型、计数、资源未就绪/失效或服务关闭错误均抛 Lua 错误。
---当前默认预算为每帧 65536 命令、64 MiB 图元顶点、16 MiB Shader 快照；超预算或不可表示的 GPU 数值会报错。
---@class Graphic
Graphic = {}

---@alias GraphicBlend 'mul+alpha'|'mul+add'|'mul+sub'|'mul+rev'|'mul+min'|'mul+max'|'mul+mul'|'mul+screen'|'add+alpha'|'add+add'|'add+sub'|'add+rev'|'add+min'|'add+max'|'add+mul'|'add+screen'|'alpha+bal'|'replace'
---@alias GraphicFogBlend 'lerp+keep'|'lerp+mul'|'lerp+fade'|'add+keep'|'add+mul'|'add+fade'|'sub+keep'|'sub+mul'|'sub+fade'|'mul+keep'|'mul+mul'|'mul+fade'|'screen+keep'|'screen+mul'|'screen+fade'
---@alias GraphicSource string|GraphicCanvas|GraphicCanvas3D
---@alias GraphicPoint2 number[] 恰好 {x,y} 的无元表连续数组。
---@alias GraphicPoint3 number[] 恰好 {x,y,z} 的无元表连续数组。
---@alias GraphicVertex2 number[] 恰好 {x,y,u,v,color} 的无元表连续数组。
---@alias GraphicVertex3 number[] 恰好 {x,y,z,u,v,color} 的无元表连续数组。
---@alias GraphicShaderValue number|number[]|number[][]|string|GraphicCanvas|GraphicCanvas3D
---@alias GraphicShaderParams table<string,GraphicShaderValue>

---合成 0xAARRGGBB 数值，不提交绘制；可以在初始化时使用。没有全局 Color 别名。
---通道须为完整 0..255 整数，缺项、额外参数、负值或越界均报错，不静默截断。
---@param a integer Alpha。
---@param r integer 红。
---@param g integer 绿。
---@param b integer 蓝。
---@return integer argb
function Graphic.Color(a, r, g, b) end

---离屏和窗口画布均为原生 userdata。对象本身不支持自定义字段；GC 只回收 Lua 包装，不释放仍驻留的离屏附件。
---请对不再需要的离屏画布显式 Destroy。重复取得同一活着的画布包装保留身份；旧代次不会指向复用的新画布。
---@class GraphicCanvas
local Canvas = {}

---创建持久 RGBA8 离屏画布；首次使用前透明黑初始化，此后内容跨帧保留，不自动合成到窗口。
---只接受 2 个参数，或全部 6 个参数；不能用 6 个 nil 代替省略边界。边界可反向，但每轴范围须非零且有限。
---默认逻辑坐标居中，X 向右、Y 向上；像素尺寸与逻辑边界独立，创建后没有 Resize/SetBounds 接口。
---当前默认最多 256 个离屏 Canvas，单边 16384 像素，附件总预算 256 MiB（含在途回收）；2D 每像素 4 字节。
---可在初始化时创建；非法尺寸/边界、预算耗尽或 GPU 分配失败报错。
---@param width integer 正整数像素。
---@param height integer 正整数像素。
---@param left? number 默认 -width/2。
---@param right? number 默认 width/2。
---@param bottom? number 默认 -height/2。
---@param top? number 默认 height/2。
---@return GraphicCanvas canvas
function Graphic.Canvas(width, height, left, right, bottom, top) end

---Canvas3D 的相机代理。只接受下列五种属性；未知属性或替换 Camera 代理本身会报错。
---位置写入立即更新配置；方向必须以完整无元表三元素数组赋值，读取 look/up 返回副本，改副本元素不写回。
---look/up 各自必须有限且非零，联合平行/近乎平行在绘制构建相机时检查；不用事先手动正交化。
---右手观察基为 forward=normalize(look)、right=normalize(look×up)，up 自动正交化；look 是方向而非目标点。
---可在初始化时修改；当前绘制目标的真实状态变化形成屏障，只影响后续命令，不重新计算旧颜色/深度/距离。
---@class GraphicCamera
---@field x number 世界位置 X，默认 0，必须有限。
---@field y number 世界位置 Y，默认 0，必须有限。
---@field z number 世界位置 Z，默认 0，必须有限。
---@field look GraphicPoint3 观察方向，默认 {1,0,0}。
---@field up GraphicPoint3 头顶方向，默认 {0,1,0}。

---@class GraphicCanvas3D: GraphicCanvas
---@field Camera GraphicCamera 只读代理属性；其 x/y/z/look/up 可写。
local Canvas3D = {}

---创建持久 3D 画布，具有 RGBA8 预乘颜色、可采样 D32 深度和 R32 欧氏距离附件；共 12 字节/像素。
---初次使用颜色透明黑、深度 1、距离 0；默认相机见 GraphicCamera，FOV=45°、near=1、far=1000，ZBuffer/Fog 均关闭。
---宽高比始终 width/height。切换目标或跨帧保留三种附件，Clear 同时清三者。
---预算/显式销毁/在途保活同 Graphic.Canvas；只有两个必填参数，没有 2D 边界或 aspect 参数。
---普通图像绘制采样其颜色；Graphic.Render 可按 Shader 声明自动取得深度/距离。GetBounds 对此类型报错。
---GPU 不支持所需附件格式、非法尺寸或预算耗尽时抛错；可在初始化时创建。
---@param width integer 正整数像素，当前上限 16384。
---@param height integer 正整数像素，当前上限 16384。
---@return GraphicCanvas3D canvas
function Graphic.Canvas3D(width, height) end

---读取当前窗口的稳定逻辑句柄，不绑定某一帧交换链指针；窗口调整后 GetSize 返回新的实际像素尺寸。
---每个可呈现帧自动清一次不透明黑；同帧切出再返回不重清，不承诺跨帧保留窗口内容。
---WindowCanvas 禁止销毁、禁止作为任何纹理输入；可在初始化时取得句柄，截图仍受帧时机限制。
---@return GraphicCanvas windowCanvas
function Graphic.GetWindowCanvas() end

---立即读取实际像素大小，适用于普通 Canvas、Canvas3D 和窗口；不建屏障、不等待 GPU。
---离屏大小固定，窗口大小实时获取；服务关闭或失效句柄报错。不是顶层 Graphic.CanvasGetSize API。
---@return integer width
---@return integer height
function Canvas:GetSize() end

---立即读取 2D 逻辑边界；普通 Canvas 返回创建配置，窗口返回当前像素尺寸对应的居中范围。
---可在帧外查询，不改状态、不建屏障；Canvas3D、失效句柄或服务关闭报错。不是顶层 Graphic.CanvasGetBounds API。
---@return number left
---@return number right
---@return number bottom
---@return number top
function Canvas:GetBounds() end

---立即撤销离屏句柄，之后新绘制/查询拒绝使用；已接受的 GPU 命令继续保活旧附件直到安全完成。
---可在帧内外调用；重复销毁返回 false，首次成功返回 true。窗口、外来句柄或服务关闭报错。
---若销毁当前目标，后续 GetTargetCanvas/绘制会报错，需显式切换到存活目标；不隐式恢复窗口。
---@return boolean destroyed
function Canvas:Destroy() end

---配置透视投影，立即更新本画布配置，仅作用后续命令；可在初始化时调用，没有返回值。
---不清深度，不修改 Camera，不接受额外 aspect 参数；非法范围/非有限值或非 3D 画布报错。
---@param fov number 垂直视角（度），0<fov<180，默认 45。
---@param near number 近裁剪距离，0<near<far，默认 1。
---@param far number 远裁剪距离，有限，默认 1000。
function Canvas3D:SetPerspective(fov, near, far) end

---同时开启深度测试与写入，比较为 LESS_OR_EQUAL；立即更新配置，仅作用后续命令，不清附件。
---双面绘制，不自动做 Alpha Test；全透明片元也可能写深度。3D 透明顺序由 Lua 安排。
---默认关闭；可在初始化时调用，参数数目/类型或失效画布错误会报错。无返回值。
function Canvas3D:OpenZBuffer() end

---同时关闭深度测试与写入，保留旧深度和距离；近/远裁剪仍生效，不清颜色。
---立即更新配置，仅影响后续命令；默认已关闭，可在初始化时调用。失效画布报错，无返回值。
function Canvas3D:CloseZBuffer() end

---设置按片元到相机欧氏距离计算的雾，立即更新配置，只影响后续命令；默认关闭。
---SetFog(nil) 是唯一关闭签名，不能完全省略参数。模式 0 线性；1/2/3 为入平方/立方/四次；4/5/6 为出平方/立方/四次。
---start/end 必须有限且 start<end，并能在 GPU float32 中保留非零区间；不强制起点非负。
---RGB 运算与 Alpha 运算分开，雾色 Alpha 不参与 RGB 雾强度：keep 保留 Alpha，mul 乘 (1-t+t*fogAlpha)，fade 乘 (1-t)。
---Straight RGB 经雾处理后按最终 Alpha 预乘。自定义 Shader 在 PSMain 前收到雾化的 input.color，而不是返回值后补雾。
---可在初始化时配置，无返回值；非法枚举/范围/颜色、失效画布报错，Fog Blend 与图元 Blend 不通用。
---@overload fun(self: GraphicCanvas3D, disabled: nil)
---@param startDistance number
---@param endDistance number
---@param mode integer 0..6。
---@param color integer ARGB，0..4294967295。
---@param blend? GraphicFogBlend 默认 'lerp+keep'。
function Canvas3D:SetFog(startDistance, endDistance, mode, color, blend) end

---设置后续绘制目标；每次调用（即使与当前相同）都结束上一排序段，不清屏、不合成、不维护隐式栈。
---每次 FrameFunction 开始默认窗口目标。仅合法绘制阶段可调用，nil/外来/已销毁画布报错；无返回值。
---@param canvas GraphicCanvas|GraphicCanvas3D 存活的本服务窗口或离屏画布。
function Graphic.SetTargetCanvas(canvas) end

---立即返回当前目标的同一逻辑句柄，不创建画布、不清屏、不建屏障、不等待 GPU。
---仅合法绘制阶段可查询；帧外或当前目标已销毁报错。可保存局部值并通过 SetTargetCanvas 恢复。
---@return GraphicCanvas|GraphicCanvas3D canvas
function Graphic.GetTargetCanvas() end

---清除当前目标颜色；3D 同时清深度为 1、距离为 0，不重置 Camera/Perspective/Fog/ZBuffer 配置。
---命令有序提交并阻止 layer 穿越；实际执行由当前帧调度。仅合法绘制阶段可用，无返回值。
---错误阶段、失效目标、非法颜色或命令预算耗尽抛错。
---@param color? integer ARGB，默认 0xff000000（不透明黑），透明清除需显式传 0x00000000。
function Graphic.Clear(color) end

---同步读取指定画布完整 PNG 二进制字符串；必须显式给画布，不写文件、不注册 Image。
---离屏 Canvas/Canvas3D 在初始化、帧内、退出回调均可读取（图形服务须存活）；WindowCanvas 仅当前可呈现帧且 Present 前可读。
---包含此前已记录的 Clear/图元/Shader，封闭排序段并等待必要 GPU Fence；可继续在同帧提交后续绘制。
---不改变当前目标、相机、Fog、ZBuffer 或隐式清屏状态；截取结果是独立副本，后续修改/销毁不改变字节。
---输出固定 RGBA8 PNG，视觉左上为首行、实际像素尺寸；内部预乘还原 Straight RGBA，零 Alpha 的 RGB=0。
---离屏保留 Alpha，3D 仅颜色、不导出深度/距离，窗口 Alpha 固定 255；不额外改变固定 sRGB 颜色解释。
---参数错误、外来/过期句柄、窗口无有效可呈现帧立即抛错；合法操作的 GPU 读回/编码失败记录日志并返回单个 nil。
---当前读回与 PNG 各有 256 MiB 预算、GPU 等待超时默认 30 秒；严重设备失败可能使后续图形操作报错。
---这是显式昂贵同步操作；需持久化时用 File.Open(path,'wb') 后 Write/Close，不提供 SaveCanvasPNG。
---@param canvas GraphicCanvas|GraphicCanvas3D
---@return string|nil png 成功完整 PNG 字节，失败 nil；始终仅一个返回值。
function Graphic.GetCanvasPNG(canvas) end

---向 2D 目标提交图像；image 为已就绪 Image 名称或本服务离屏 Canvas/Canvas3D，不是路径或 Image userdata。
---禁止 WindowCanvas/当前目标自采样。坐标 X 右 Y 上，rotation 按度、逆时针为正；Image UV 原点在左上。
---Resource 的中心与固有缩放各应用一次，再乘本次 sx/sy；Canvas 源使用像素大小、几何中心、linear+clamp。
---负缩放镜像，0 缩放退化而无可见面积；颜色/Blend/layer 仅作用本次命令，默认值见参数。
---调用时捕获源元数据和绘制参数；帧外、3D 目标、非法参数/源或超预算报错，无返回值。
---@param image GraphicSource
---@param x number 图像锚点的目标逻辑坐标。
---@param y number 图像锚点的目标逻辑坐标。
---@param rotation? number 默认 0 度。
---@param sx? number 有限数，默认 1。
---@param sy? number 有限数，默认 sx。
---@param color? integer ARGB，默认 0xffffffff。
---@param blend? GraphicBlend 默认 'mul+alpha'，不接受空串或其他别名。
---@param layer? number 有限数，默认 0。
function Graphic.RenderImage(image, x, y, rotation, sx, sy, color, blend, layer) end

---向 2D 目标提交完整图像矩形，显式几何不使用 Image 中心或固有缩放；边界可反向以镜像，也可退化。
---逻辑映射为右上、左上、左下、右下；图像输入/禁自采样/生命周期/错误规则同 RenderImage。
---只记录本次命令，不改资源；帧外或 3D 目标报错，无返回值。
---@param image GraphicSource
---@param left number 有限逻辑坐标。
---@param right number 有限逻辑坐标。
---@param bottom number 有限逻辑坐标。
---@param top number 有限逻辑坐标。
---@param color? integer 默认 0xffffffff。
---@param blend? GraphicBlend 默认 'mul+alpha'。
---@param layer? number 有限数，默认 0。
function Graphic.RenderRect(image, left, right, bottom, top, color, blend, layer) end

---提交四边形，按轮廓顺序传四点；对应完整图像的右上、左上、左下、右下，不应用资源中心/固有缩放。
---当前目标决定顶点维数：2D 是 {x,y}，Canvas3D 是 {x,y,z}；数组不可带元表、缺项或附加字段。
---几何须严格凸、无重复/共线角/自交，可采用任一绕序；3D 另须共面（相对容差 1e-8），近远裁剪不受 ZBuffer 开关影响。
---使用全四边形射影 UV，非法/数值病态几何报错；颜色统一。3D 不接受 layer 参数，即使显式 nil 也报错。
---只在合法绘制阶段提交；输入与禁自采样规则同 RenderImage，状态按调用时固定，无返回值。
---@overload fun(image: GraphicSource, p1: GraphicPoint3, p2: GraphicPoint3, p3: GraphicPoint3, p4: GraphicPoint3, color?: integer, blend?: GraphicBlend)
---@param image GraphicSource
---@param p1 GraphicPoint2
---@param p2 GraphicPoint2
---@param p3 GraphicPoint2
---@param p4 GraphicPoint2
---@param color? integer 默认 0xffffffff。
---@param blend? GraphicBlend 默认 'mul+alpha'。
---@param layer? number 仅 2D，有限数，默认 0。
function Graphic.RenderQuad(image, p1, p2, p3, p4, color, blend, layer) end

---提交带显式 UV 与逐顶点颜色的四边形；2D 顶点恰好 {x,y,u,v,color}，3D 恰好 {x,y,z,u,v,color}，没有顶点默认值。
---几何和原始数组校验同 RenderQuad；3D 没有 layer 参数（额外 nil 也不接受）。每点 color 是 uint32 ARGB。
---u/v 为逻辑 Image/Canvas 内部归一化坐标，(0,0) 左上、(1,1) 右下；不是 PNG 像素或 Atlas 页 UV。
---有限越界 UV 按源 Image 的 clamp/wrap 与独立 mip 处理；Canvas 为 linear+clamp，不应用 Image 中心/缩放。
---UV 射影插值，常量及可解的一维仿射 UV 可用；无唯一映射、内部极点、数值病态报错。颜色按三角形插值。
---调用时复制顶点，仅合法绘制阶段可用；输入源/禁自采样/错误规则同 RenderImage，无返回值。
---@overload fun(image: GraphicSource, v1: GraphicVertex3, v2: GraphicVertex3, v3: GraphicVertex3, v4: GraphicVertex3, blend?: GraphicBlend)
---@param image GraphicSource
---@param v1 GraphicVertex2
---@param v2 GraphicVertex2
---@param v3 GraphicVertex2
---@param v4 GraphicVertex2
---@param blend? GraphicBlend 默认 'mul+alpha'。
---@param layer? number 仅 2D，有限数，默认 0。
function Graphic.Render4V(image, v1, v2, v3, v4, blend, layer) end

---提交一条 2D 带状图元，与 Object/碰撞无关；points 是无元表连续数组，每点恰好 {x,y}，最多 16384 个输入点。
---width 为逻辑空间的完整宽度，有限且 >0，不乘资源中心/缩放。开放默认平直端帽；closed=true 自动连接末点与首点。
---合并相邻近重复点（以及闭合重复末点）；开放至少 2 个有效点，闭合至少 3 个非共线有效点。
---稀疏/多余字段、非法坐标、完全退化或 180° 回折报错。miter 上限半宽的 4 倍，超限外侧 bevel、内侧回到中心线。
---自交/过宽路径可能重叠，按路径顺序混合，不做布尔并集。u 从路径左侧 0 到右侧 1，v 为累计弧长/总长。
---闭合长度包括末段，首尾截面相同而 v 为 0/1；材质两端不一致仍可能显接缝，Atlas wrap 仅在逻辑切片内。
---color/blend/layer 均必填。点列在调用时复制为一个可排序命令；只在 2D 目标合法绘制阶段使用，无返回值。
---输入和禁自采样规则同 RenderImage；错误类型、宽度、几何、目标或预算在调用处报错。
---@param image GraphicSource
---@param points GraphicPoint2[]
---@param width number 完整宽度，>0。
---@param color integer ARGB。
---@param blend GraphicBlend 必须显式传字符串。
---@param layer number 必须显式传有限数。
---@param closed? boolean 默认 false；不接受数字替代布尔。
function Graphic.RenderRibbon(image, points, width, color, blend, layer, closed) end

---不可变的顶点/索引原生副本，无可写字段和 Update/Destroy 方法；GC 释放 CPU 句柄，已接受绘制仍自行保活。
---@class GraphicMesh

---创建静态 triangle-list 网格，可在初始化时完成，之后重复作为 Graphic.Render 的末参数使用，不自动绘制。
---两参数均为无元表连续数组；每顶点恰好 {x,y,z,u,v,color}，2D 也需显式给 z=0，数值 x/y/z/u/v 须可表示为有限 float32。
---至少 3 顶点；索引为 Lua 1-based，范围 1..#vertices，数量非零且为 3 的倍数。允许退化三角，不要求凸/共面。
---当前最多 1048576 顶点、3145728 索引；非法数组、颜色/索引范围、超容量或内存分配失败抛错。
---调用立即复制数据，后续修改输入 table 不影响 Mesh；不是 Resource 资源名，也不占 Object 池。
---@param vertices GraphicVertex3[] 6 元素顶点，其中 color 为 uint32 ARGB。
---@param indices integer[] 三角形索引列表。
---@return GraphicMesh mesh
function Graphic.Mesh(vertices, indices) end

---以已就绪 Shader 名称提交绘制；不加载磁盘文件、不设置全局材质状态；仅合法 FrameFunction 绘制阶段可用。
---2D/窗口签名为 Render(shader,params,layer?,mesh?)；3D 为 Render(shader,params,mesh?)，布局只由当前目标决定。
---3D 不接受第四参数（包括 nil）；2D 要在第四项传 Mesh，第三项须先给 layer 或 nil，不能直接移位传 Mesh。
---省略/nil Mesh 使用覆盖整个目标的超大全屏三角形，UV 左上 (0,0)、右下 (1,1)；显式 Mesh 使用其顶点/索引。
---默认 VS 在 2D 应用 Canvas 逻辑边界、3D 应用当前 Camera 的 ViewProjection；自定义 VS 负责自身变换，不二次套相机。
---
---params 必须是无元表普通 table，键为大小写敏感的 HLSL 字段名；没有用户参数也必须传 {}，不接受 nil。
---支持 float、float2/3/4、int32、uint32、float4x4 和 Texture2D；浮点须有限且在 float32 范围，整数必须无小数且不越界。
---向量为恰好 2/3/4 项的无元表连续数组，float4 不做 ARGB 解包；矩阵为按行填写的 4×4 数组，内部转换 HLSL 列主序。
---VS/PS 编译后实际使用的用户字段必须提供；已声明但优化掉的字段可省略，若提供仍严格校验，不保留上一 Draw 的参数。
---未知字段、保留的 Solongo 字段、Sampler 字段、错误类型、数组缺项/附加项/元表、非有限值均报错。
---
---Texture2D 值只接受就绪 Image 名称或存活离屏 Canvas/Canvas3D；不接受路径、WindowCanvas 或当前输出的任一附件。
---Image 采样限定逻辑切片 0..1，沿用调用时的 sampler 和 mip，不应用中心/固有缩放；Canvas 完整像素为 linear+clamp。
---若 HLSL 声明主 foo 及 foo_depth/foo_distance，Lua 只填 foo=Canvas3D，派生键禁止覆盖；深度/距离默认 point+clamp。
---即使派生声明被优化掉，显式主输入仍须为 Canvas3D；被优化但提供的所有纹理也会进行存活/类型/禁自采样检查。
---深度背景值 1，范围 0..1；距离为最近有效深度写入时到相机的欧氏距离，无表面为 0。修改相机不重算历史附件。
---
---用户 Texture2D 采样为 Straight RGBA；Canvas 内部预乘颜色按依赖生成 Straight 视图，Alpha=0 时 RGB=0。
---2D 的 input.color 为未雾化顶点 tint；3D 基础色为 tint，若声明标准 Texture2D texture 再乘其采样，然后应用目标 Fog。
---Fog 结果写入 input.color 后才调用 PSMain；用户可忽略该颜色。PSMain 的 Straight 返回值最终预乘一次并以 mul+alpha 混合。
---3D Mesh 雾取片元世界位置；全屏处理读取目标历史距离，距离 0 跳过雾。全屏保持原深度/距离，不用全屏几何造距离。
---Mesh 仅在 ZBuffer 开启且通过深度测试时更新深度/距离；关闭时保留旧值。采样源 Canvas3D 不会重算源 Fog。
---
---调用时复制 params 并固定 Camera/Fog/ZBuffer/Image 元数据；后改 Lua table/资源元数据不影响已接受命令。
---2D layer 与其他图元共同稳定排序，SetTargetCanvas/Clear/当前目标状态变化/GetCanvasPNG 的屏障不可穿越。
---Resource.Unload/Canvas.Destroy 阻止后续使用，但不撤销已接受的旧代次 Shader/Image/Canvas/Mesh 引用。
---未知/未就绪 Shader、非法签名/参数/源、状态数值不可表示或预算耗尽在调用处报错；成功无返回值。
---@overload fun(shader: string, params: GraphicShaderParams, mesh?: GraphicMesh)
---@param shader string 1..512 字节、不含 NUL 的已就绪 Shader 资源名称。
---@param params GraphicShaderParams 当前 Draw 的完整用户字段。
---@param layer? number 仅 2D，有限数，默认 0。
---@param mesh? GraphicMesh 仅 2D 的第四项；3D 则在第三项。省略/nil 为全屏绘制。
function Graphic.Render(shader, params, layer, mesh) end
