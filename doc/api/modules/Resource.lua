---@meta

---Selen2D 2.0.0 一号机资源接口，依据基线 42585671780bca75692550685e49dcb824e2d36e 绑定及实现核对。
---名称跨 Image/Sound/Music/Shader/Font 全局唯一；name 是 1..512 字节且不含 NUL 的字符串。
---path 是 1..4096 字节且不含 NUL 的 VFS 逻辑路径；相对虚拟根、使用 /，遵循 launch 挂载覆盖和路径权限规则。
---参数个数/类型错误、重复名、未知名、正在加载或资源类型不符均抛 Lua 错误；不返回伪默认值。
---同步 LoadXXX 成功返回时全部准备已完成，可能阻塞读取/解码/编译和必要 GPU Fence；不执行 Lua 回调或推进 Object。
---每个资源驻留到 Unload 或退出；Canvas 属于 Graphic，普通字节/JSON 文件由 File/Text 处理。
---本文件仅为编辑器声明，不应执行；没有公开 Image userdata、Manifest、Atlas、Job 或 Cancel API。
---@class Resource
Resource = {}

---@alias ImageSampler 'point+clamp'|'linear+clamp'|'point+wrap'|'linear+wrap'

---增量收集器，可跨帧登记；不是资源所有者。GC 不卸载已完成资源，也不取消已提交任务。
---@class ResourceLoader
local Loader = {}

---创建空 Loader，立即返回，不读取文件。只可成功提交一次。
---@return ResourceLoader loader
function Resource.CreateLoader() end

---同步加载 PNG；裁剪坐标以源图左上为原点、右/下为正，裁剪范围须完整落在源图内。
---u/v/w/h 全部省略或 nil 表示整图，否则四项必须一起提供。centerX/centerY 也必须成对给出。
---默认中心为裁剪图几何中心，scale=1、mipmap=false、sampler='linear+clamp'；不开始绘制，无返回值。
---非法裁剪、图片格式/读取/解码/上传失败报错且不残留名称。固有缩放允许负数和 0，不改变逻辑像素尺寸。
---@param name string 全局唯一名称。
---@param path string PNG 的 VFS 路径。
---@param u? integer 非负整数像素，最大 4294967295。
---@param v? integer 非负整数像素，最大 4294967295。
---@param w? integer 正整数像素，最大 4294967295。
---@param h? integer 正整数像素，最大 4294967295。
---@param centerX? number 相对裁剪图左上角的有限像素坐标，可在图外；默认 w/2。
---@param centerY? number 相对裁剪图左上角的有限像素坐标，可在图外；默认 h/2。
---@param scale? number 有限的初始等比固有缩放，默认 1。
---@param mipmap? boolean 默认 false；只接受布尔值。
function Resource.LoadImage(name, path, u, v, w, h, centerX, centerY, scale, mipmap) end

---只登记图片请求，不读文件；参数、裁剪规则与 Resource.LoadImage 相同，默认整图、几何中心、scale=1、mipmap=false。
---本批重复名/非法参数立即报错；读取、源图边界、解码和 GPU 失败通过提交后的 IsFinished 报错。无返回值。
---@param name string
---@param path string
---@param u? integer 非负整像素，须与 v/w/h 同时提供。
---@param v? integer 非负整像素。
---@param w? integer 正整像素。
---@param h? integer 正整像素。
---@param centerX? number 有限数，与 centerY 成对；默认裁剪图中心。
---@param centerY? number 有限数。
---@param scale? number 默认 1，允许负值或 0。
---@param mipmap? boolean 默认 false。
function Loader:LoadImage(name, path, u, v, w, h, centerX, centerY, scale, mipmap) end

---同步加载短音效并完整解码到共享 PCM；支持按文件头识别的 WAV/Vorbis，初始资源音量为 1。
---成功后可供 Audio.PlaySound 使用，不自动播放；读取/解码失败报错。无返回值。
---@param name string
---@param path string
function Resource.LoadSound(name, path) end

---登记完整解码音效请求，格式与默认音量同 Resource.LoadSound；提交前不读文件、不播放。
---重复名或提交后继续登记报错；后台失败由 IsFinished 报告。无返回值。
---@param name string
---@param path string
function Loader:LoadSound(name, path) end

---同步准备流式音乐；不要求整首 PCM 常驻，不自动播放，初始资源音量为 1。
---循环点单位为源音频秒，默认 [0,源结尾)；必须 0<=loopStart<loopEnd<=源时长，内部转换为 PCM 帧。
---读取/格式/循环区间无效时抛错；成功后 Audio.PlayMusic 默认按此区间循环。无返回值。
---@param name string
---@param path string WAV/Vorbis VFS 路径，按文件头识别。
---@param loopStart? number 有限非负数，默认 0。
---@param loopEnd? number 有限数，默认源音频结尾。
function Resource.LoadMusic(name, path, loopStart, loopEnd) end

---登记流式音乐，默认循环起点 0、终点源结尾、音量 1；不立即读取或播放。
---提交后异步校验源时长与循环区间，失败在 IsFinished 报错；提交后禁止添加。无返回值。
---@param name string
---@param path string
---@param loopStart? number 有限非负秒数，默认 0。
---@param loopEnd? number 有限秒数，默认源结尾，须大于 loopStart 且不超过源时长。
function Loader:LoadMusic(name, path, loopStart, loopEnd) end

---同步读取并校验 TTF/OTF 字体，成功后注册名称；不支持 .fnt，也不提供字形绘制/排版接口。
---加载完成只表示文件和格式准备好，不隐式栅格化文本。格式/读取失败报错，无返回值。
---@param name string
---@param path string
function Resource.LoadFont(name, path) end

---登记 TTF/OTF 请求，提交前不读文件；无字体绘制副作用，后台格式/读取失败由 IsFinished 报错。
---提交后继续登记或本批重复名报错。无返回值。
---@param name string
---@param path string
function Loader:LoadFont(name, path) end

---同步加载 HLSL，经 VFS 解析文件及相对 include；须 include "Solongo.hlsl" 并定义 PSMain，可选 VSMain。
---编译、反射、GPU Shader 和基础管线准备均成功才发布名称；失败包含资源诊断且不留下半成品。
---此基线采用 Windows DXIL，HLSL 编译器静态链接在引擎内，运行不需要外部 DXC DLL 或 SDK。
---这里只准备 Shader；绘制与参数类型由 Graphic.Render 定义。无返回值。
---@param name string
---@param path string
function Resource.LoadShader(name, path) end

---登记 HLSL 请求；ABI、VFS include 和平台限制同 Resource.LoadShader，不在登记时编译。
---编译/反射/GPU 准备失败通过 IsFinished 报错；单次 DXC 调用不能被中断。无返回值。
---@param name string
---@param path string
function Loader:LoadShader(name, path) end

---原子检查并预留本批全部名称，开始异步处理，不等待读取、解码或 GPU 完成。无返回值。
---成功后封存，继续添加/重复提交报错；同步名称冲突不封存，可解决外部冲突后重试。
---与已就绪资源或其他批次的预留名称冲突会立即报错；批内重复名在登记时即已报错。
---全部准备成功才一次性公开本批资源；任何后台失败都撤销预留且不发布部分资源，失败批次不能再提交。
function Loader:Submit() end

---非阻塞查询；未提交或仍处理中为 false，整批准备及必要 GPU 同步成功后为 true。
---失败终态每次查询都抛相同的名称、路径及原因；失败重试须新建 Loader。服务已关闭/句柄非法报错。
---@return boolean finished
function Loader:IsFinished() end

---立即返回已就绪 Image 的裁剪像素尺寸，与固有缩放、中心和 Atlas 物理位置无关。
---未知、未就绪或非 Image 名称抛错；不改变绘制状态。
---@param name string
---@return integer width
---@return integer height
function Resource.GetImageSize(name) end

---立即更新当前 Image 中心；只影响之后提交的绘制，已提交命令保留旧快照。
---允许图外/负坐标；非有限数、未知/未就绪/非 Image 报错，无返回值。
---@param name string
---@param centerX number 相对裁剪图左上角的像素坐标。
---@param centerY number 相对裁剪图左上角的像素坐标。
function Resource.SetImageCenter(name, centerX, centerY) end

---返回当前中心元数据，加载时未指定则为裁剪图几何中心；不代表历史命令的快照。
---未知、未就绪或非 Image 报错。
---@param name string
---@return number centerX
---@return number centerY
function Resource.GetImageCenter(name) end

---立即更新 Image 两轴固有缩放，只影响后续提交；允许负值镜像或 0 退化，禁止 NaN/Inf。
---RenderImage 将其与绘制缩放各乘一次，显式几何及 Shader 采样不使用它；不改变图像像素宽高。无返回值。
---@param name string 已就绪 Image 名称，否则报错。
---@param hscale number 有限数。
---@param vscale? number 省略/nil 时等于 hscale。
function Resource.SetImageScale(name, hscale, vscale) end

---返回当前横、纵缩放，始终两个值；默认均 1，加载时 scale 可覆盖。未知/未就绪/非 Image 报错。
---@param name string
---@return number hscale
---@return number vscale
function Resource.GetImageScale(name) end

---立即设置逻辑 Image 的过滤/寻址；默认 linear+clamp，仅影响后续绘制，已排队命令保留原采样快照。
---Atlas 子图及其 Mipmap 的 wrap 只重复逻辑切片，不采样整页邻图。未知枚举/未就绪/非 Image 报错，无返回值。
---@param name string
---@param sampler ImageSampler
function Resource.SetImageSampler(name, sampler) end

---立即返回当前采样器枚举，默认 linear+clamp；不返回 GPU 句柄。未知/未就绪/非 Image 报错。
---@param name string
---@return ImageSampler sampler
function Resource.GetImageSampler(name) end

---立即修改 Sound/Music 的资源音量，同时影响已有和未来播放实例；最终增益另乘 Audio 对应分类音量。
---不接受 Image/Shader/Font 或未知/未就绪名称；越界/非有限 volume 报错。无返回值。
---@param name string
---@param volume number 有限数，范围 [0,1]，加载默认 1。
function Resource.SetVolume(name, volume) end

---返回音频资源自身配置的增益（默认 1），不乘分类音量。非 Sound/Music、未知或未就绪名称报错。
---@param name string
---@return number volume [0,1]。
function Resource.GetVolume(name) end

---立即撤销资源名并停止关联音频；此后按名使用报错，同名可以重新加载。
---已接受的 GPU 命令保活旧 Image/Shader 到安全完成，不被新同名资源替换；没有返回值。
---不存在或正在加载的名称报错；不接受 Loader、Canvas 或磁盘路径。
---@param name string 已就绪资源的名称。
function Resource.Unload(name) end
