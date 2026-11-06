---@meta

---资源名称跨 Image/Sound/Music/Font/Shader 全局唯一，1..512 字节且不含 NUL。
---path 是编译期授权的 VFS 路径；登记即检查语法和权限。非法参数、重名、未就绪及类型错误抛错。
---同步加载可能等待 I/O、解码、编译及 GPU Fence，不执行 Lua 游戏回调。资源存活至 Unload 或退出。
---本文件仅为编辑器声明，不应执行；没有公开 Texture、Group、Job 或 Cancel API。
---@class Resource
Resource = {}

---@alias ResourceType 'Image'|'Sound'|'Music'|'Font'|'Shader'
---@alias ImageSampler 'point+clamp'|'linear+clamp'|'point+wrap'|'linear+wrap'

---只读表自身字段，不执行元方法、不补写默认值；未知键、数字键、数字字符串、scale 别名均报错。
---@class ResourceImageOptions
---@field u? number 有限且 >=0，默认 0；源图左上原点向右。
---@field v? number 有限且 >=0，默认 0；源图左上原点向下。
---@field w? number 有限且 >0，默认源宽-u；保留小数，不取整。
---@field h? number 有限且 >0，默认源高-v；各轴独立缺省。
---@field centerX? number 有限数，默认最终 w/2；相对切片，可在图外。
---@field centerY? number 有限数，默认最终 h/2；与 centerX 独立缺省。
---@field hscale? number 有限数，默认 1，允许正、负、0。
---@field vscale? number 有限数，默认最终 hscale，允许正、负、0。
---@field mipmap? boolean 默认 false；不改变逻辑尺寸、中心及缩放。

---@class ResourceMusicOptions
---@field loopStart? number 有限且 >=0，默认 0 秒。
---@field loopEnd? number 有限数，默认源时长；要求 loopStart < loopEnd <= 源时长。

---增量收集器，可跨帧登记；GC 不卸载资源、不取消已提交任务。
---@class ResourceLoader
local Loader = {}

---创建空 Loader，立即返回，不读文件。
---@return ResourceLoader loader
function Resource.CreateLoader() end

---同步加载 PNG；省略/nil/空 options 均采用缺省。不接受第四参数或旧长位置参数。
---各裁剪/中心字段独立可选，区域必须位于源图内；固有缩放不改变裁剪尺寸、中心或 UV。
---静态参数立即验证，实际边界/解码/Atlas/GPU 失败抛错，不公开半成品。
---@param name string
---@param path string
---@param options? ResourceImageOptions
function Resource.LoadImage(name, path, options) end

---登记即复制 options 字段和值及缺省标记；之后改表、清空或 GC 不影响请求。
---静态参数、路径、本批重名立即报错，不占失败请求的名字；内容错误由提交后查询报告。
---@param name string
---@param path string
---@param options? ResourceImageOptions
function Loader:LoadImage(name, path, options) end

---完整解码 WAV/Vorbis 音效，不自动播放；资源音量初始 1。只接受两个参数。
---@param name string
---@param path string
function Resource.LoadSound(name, path) end

---登记完整解码音效；只接受 name/path，提交前不读文件。
---@param name string
---@param path string
function Loader:LoadSound(name, path) end

---准备流式 WAV/Vorbis 音乐，不自动播放，不保存整首 PCM；资源音量初始 1。
---默认整段循环；首次从 0 播至 loopEnd，后续重复 [loopStart,loopEnd)，内部换算为源 PCM 帧。
---起点/终点独立缺省，时长越界在加载时抛错；不接受第四参数。
---@param name string
---@param path string
---@param options? ResourceMusicOptions
function Resource.LoadMusic(name, path, options) end

---登记流式音乐并复制 options 快照；不读取元方法，内容错误由提交后查询报告。
---@param name string
---@param path string
---@param options? ResourceMusicOptions
function Loader:LoadMusic(name, path, options) end

---同步准备独立可缩放 TTF/OTF 字体，不接受集合、位图字库、WOFF 或 options 重载。
---完成时字体字节和解析已验证，不预栅格化整个 Unicode 字库。
---@param name string
---@param path string
function Resource.LoadFont(name, path) end

---登记 TTF/OTF，解析失败由提交后查询报告。
---@param name string
---@param path string
function Loader:LoadFont(name, path) end

---HLSL 和相对 include 统一经 VFS；编译、反射、GPU 程序准备成功才发布。
---公开 ABI 由当前 Graphic 声明和 Shader 指南确定；不定义另一套 Shader ABI。
---编译器使用 EXE 同目录随包提供的 dxcompiler.dll，运行无需安装 Python 或 SDK。错误含名称、路径和诊断。
---@param name string
---@param path string
function Resource.LoadShader(name, path) end

---登记 HLSL，不立即编译；后台编译/反射/GPU 错误由提交后查询报告。
---@param name string
---@param path string
function Loader:LoadShader(name, path) end

---原子预留本批名字，非阻塞开始准备；接受后封存，只可接受一次 Submit。
---同步名字冲突不接受该次提交，可解决冲突后重试。空批次接受时立即完成。
---非空批次全部成功才原子公开；失败释放预留、不公开任何资源，重试必须使用新 Loader。
function Loader:Submit() end

---未提交或处理中 false；整批公开且必要 GPU Fence/字体准备已完成才 true。
---失败终态每次抛同一名称/路径/原因。非阻塞查询，不推进工作、不等待 GPU。
---@return boolean finished
function Loader:IsFinished() end

---未提交为 0；N 项请求中 C 项真正准备完成但未发布时 C/(N+1)，严格小于 1。
---整批发布成功为 1（包括空批次），失败与 IsFinished 一样重复抛错。
---不是耗时/字节比例或可用资源数；仅观察完成记录，不要求窗口 Present。
---@return number progress 有限值 [0,1]。
function Loader:GetProgress() end

---返回未缩放的精确逻辑裁剪宽高，可为小数；不是 Atlas 像素覆盖尺寸。
---未知、未就绪或非 Image 名称抛错。
---@param name string
---@return number width
---@return number height
function Resource.GetImageSize(name) end

---更新中心，允许图外坐标；只影响之后提交的命令，旧 Draw 保留快照。
---@param name string 已就绪 Image。
---@param centerX number 有限数，相对切片左上。
---@param centerY number 有限数。
function Resource.SetImageCenter(name, centerX, centerY) end

---@param name string 已就绪 Image；未知/类型错误/未就绪抛错。
---@return number centerX
---@return number centerY
function Resource.GetImageCenter(name) end

---更新固有缩放，负数镜像、0 退化；不改尺寸/中心/UV，只影响后续 Draw。
---RenderImage 与本次 sx/sy 各乘一次；RenderRect 显式边界不使用固有中心/缩放。
---@param name string 已就绪 Image。
---@param hscale number 有限数。
---@param vscale? number 有限数，省略/nil 时等于 hscale。
function Resource.SetImageScale(name, hscale, vscale) end

---@param name string 已就绪 Image。
---@return number hscale
---@return number vscale
function Resource.GetImageScale(name) end

---设置逻辑 Image 采样器，默认 linear+clamp；旧 Draw 保留旧值，不改变其他 Image。
---错误枚举、未知/未就绪名称或类型不符抛错。
---@param name string
---@param sampler ImageSampler
function Resource.SetImageSampler(name, sampler) end

---@param name string 已就绪 Image。
---@return ImageSampler sampler
function Resource.GetImageSampler(name) end

---Sound/Music 资源音量立即影响所有已有和未来播放器，最终增益另乘播放器音量。
---越界、非有限数、未知/未就绪或非音频资源抛错。
---@param name string
---@param volume number 有限数 [0,1]，加载默认 1。
function Resource.SetVolume(name, volume) end

---@param name string 已就绪 Sound/Music。
---@return number volume 资源音量，不乘播放器音量。
function Resource.GetVolume(name) end

---只查已发布资源；未知、未提交、预留未发布、失败、卸载名称返回 nil。
---不读文件、不推进工作、不发布；非法 name 类型仍抛错。
---@param name string
---@return ResourceType? type
function Resource.GetType(name) end

---立即撤销已就绪名称；不存在/正在加载时报错。同名重载属于新代次。
---旧 Draw 保活原资源至 GPU 安全完成；音频停止全部播放器的该代次并清除去重记录。
---@param name string
function Resource.Unload(name) end
