# 示例 Shader

`example/main.lua` 在启动时调用：

```lua
Resource.LoadShader('example.effect', 'example/effect.hlsl')
```

这是同步资源加载：读取源码与 include，进行运行时编译、反射及 GPU 资源准备，成功后才发布资源名。编译失败会报告资源诊断，示例不会换用另一套画面掩盖错误。运行时无需调用 `dxc.exe`，也不读取 SDK、tools 或 Python；编译器、内部验证器、反射和语义分析共用随包 `dxcompiler.dll` 的工厂。部署时保留与 EXE 同配置的 DLL。

引擎仅从原生 EXE 绝对目录加载 `dxcompiler.dll`，不从当前目录、PATH 或 SDK 寻找替代版本。缺失、损坏、错误架构或缺少工厂导出会导致明确的初始化失败；原生诊断包含预期路径和系统错误码，写往可用标准错误流与调试器，不进入 `Debug.GetLog()` 的 Lua 缓存。此加载路径不改变 Shader 的 VFS include 授权或 `Engine.hlsl` 接口。源码构建中的 `ENGINE_DXC_EXECUTABLE` 是生成内置字节码的独立主机工具，不参与运行时 DLL 搜索。

固定 DXC 源码版本和发布 DLL 的版本号、SHA256 是构建/打包检查项。运行时只要求系统成功加载这个绝对路径并找到工厂导出，不另外核对版本或摘要；不能把它理解为运行时版本锁定或防替换校验。请部署与 EXE 同次构建、同配置的 DLL，不自行从 SDK 或其他来源替换。

DXC 是原生依赖，加载不经过 Lua VFS。默认 `root / . → .` 授权整个安全部署根，因此同目录 DLL 也属于该映射的普通文件视图；引擎不会因扩展名而把它隐藏。若游戏需要只暴露资源目录，源码构建可在 JSON 中使用明确的非根目录映射，保持 DLL 在 EXE 旁；这不会改变原生 DXC 加载路径，也不需要额外为 DLL 增加 Lua 目录授权。

Shader 开头包含两个文件：

```hlsl
#include "Engine.hlsl"
#include "palette.hlsl"
```

`Engine.hlsl` 是引擎提供的内置公共声明，不需要在游戏目录另放一个同名文件。公共 include 名称固定为 `Engine.hlsl`，guard 为 `ENGINE_HLSL`，内建字段统一使用 `Engine` 前缀；不提供旧名称别名。不要自行重定义 `VSInput`、`PSInput` 或占用 `Engine` 前缀。

`palette.hlsl` 是实际的示例文件，通过 VFS 从 Shader 所在目录解析。它提供 `AnimatedAccent` 和 `SoftDisk` 两个辅助函数。修改该文件、保存并重启程序，就会在下一次资源加载时重新编译；加载后的 Shader 不自动监视文件变化。

像素入口签名为：

```hlsl
float4 PSMain(PSInput input) : SV_Target0
```

示例省略 `VSMain`，使用引擎默认顶点处理。Lua 帧回调中的 `Graphic.Render('example.effect', params)` 不传 Mesh，因此覆盖整个当前目标；每帧开始时默认目标是窗口。全屏 `input.uv` 左上为 `(0,0)`、右下为 `(1,1)`。Shader 利用 `EngineTargetSize` 修正宽高比，把 `center` 定义为居中、Y 向上的坐标，单位为窗口高度，因此窗口变宽不会把圆拉成椭圆。

用户数值放在唯一的 `cbuffer Params` 中，不手写 GPU `register` 绑定。Lua table 的字段名大小写必须与 HLSL 声明一致：

| HLSL 字段 | 类型 | Lua 示例 | 用途 |
|---|---|---|---|
| `elapsed` | `float` | `1.25` | 动画时间，秒 |
| `center` | `float2` | `{0.1, -0.05}` | 圆形位置 |
| `radius` | `float` | `0.12` | 圆形半径 |
| `accentColor` | `float3` | `{0.25, 0.78, 1.0}` | 主色 RGB |
| `backgroundColor` | `float3` | `{0.018, 0.028, 0.055}` | 背景 RGB |

这些值经过实际编译反射后按字段布局上传。向量必须是恰好对应数量的连续数组，参数 table 和向量都不能带元表；不能把 ARGB 整数当作 `float3/float4`。未知字段、错误类型或缺少实际使用的字段会报错。`EngineTargetSize` 和 `EngineTargetInvSize` 由引擎提供，不要放进 Lua params。

`Graphic.Render` 在调用时复制参数，本次提交之后修改 table 只影响下一次提交。每次提交都要提供需要的字段，没有“沿用上一帧参数”的隐式状态。示例直接修改 `params.center`、`params.elapsed`，然后提交一张完整画面。

像素入口返回 Straight RGBA；引擎最终预乘一次并以既定混合方式提交。示例返回 Alpha `1.0`，绘制不透明背景和圆形，不依赖图片或额外的字体接口。

颜色纹理允许直接调用以下两种形式：

```hlsl
scene.Sample(sceneSampler, input.uv)
scene.SampleLevel(sceneSampler, input.uv, lod)
```

纹理必须是已声明的全局 `Texture2D`，sampler 必须是对应的全局 `sceneSampler`。普通表达式、宏展开和只接收数值的辅助函数可用于计算 UV/LOD；括号不改变引用身份。颜色纹理的 `Load`、`GetDimensions`、`SampleGrad`、offset/clamp/status 等其他重载，以及把颜色纹理赋给局部别名或传入资源参数的写法，当前会在加载时报告源码位置错误。不能用这些形式绕过局部裁剪或颜色转换。`Sample` 使用片元导数，顶点阶段使用显式 LOD 的 `SampleLevel`。

Image 的 UV 始终对应逻辑裁剪区域，包含小数裁剪和局部 wrap。Resource 当前的 point/linear、clamp/wrap 与 mip 设置在提交 Draw 时封存。Canvas 全部像素映射到 `0..1`，采用 linear+clamp。颜色中心、Resource 固有缩放只影响相应原生图元的几何，不改变 Shader 纹理采样区域。

颜色过滤先对预乘工作数据进行，再向用户函数返回 Straight RGBA。零 Alpha 的**过滤结果**返回透明黑；正 Alpha 不使用 epsilon 截断，反预乘得到大于 1 的 RGB 不在采样阶段裁剪。Canvas 中特殊混合留下的零 Alpha、非零 RGB 会参与过滤，不因建立采样视图而被清除。Shader 修改 Alpha 后，引擎按最终 Alpha 预乘一次。

当前图像和离屏目标采用 RGBA8_UNORM；RGB 按 sRGB 编码分量计算，不做硬件 sRGB 解码到线性光照空间。PNG 解码保留原 Straight 字节；引擎发布图片工作数据前将 RGB 换为 `round(C*A/255)` 的 8 位预乘值，Alpha 保持不变，随后在这些数据上生成 mip。此量化会损失低 Alpha 的颜色精度：例如 `(128,64,255,1)` 的工作字节为 `(1,0,1,1)`，采样反预乘约为 `(1,0,1,1/255)`，不承诺恢复原始 Straight RGB。普通 PNG 输出继续按 sRGB 编码分量解释。

Canvas3D 的任意主名 `scene` 可派生 `scene_depth` 和 `scene_distance`，各自声明配对 sampler；Lua 只传 `scene`。深度与距离保留原生数值采样，包括 `SampleLevel` 和 `Load`，不做颜色转换，默认 point+clamp；尺寸与主 Canvas3D 像素尺寸一致。声明派生输入后，即使编译优化掉读取，也必须传有效 Canvas3D。

目标为 Canvas3D 且声明标准主纹理 `texture` 时，`params.texture` 始终必填，包括最终 PS 返回常量的情况。引擎用主颜色采样乘插值 tint，再执行可选 Fog，交给用户 `PSInput.color`；用户返回后不再乘主纹理或补雾。普通 2D 目标的 `input.color` 仍是插值顶点色。

源码和普通 include 均经 VFS 授权；include 禁止绝对路径及独立 `.`、`..` 组件，大小写严格匹配。内置 `Engine.hlsl` 优先于同名资源文件。引擎用固定 DXC 的语义引用和源范围识别上述颜色访问，生成内部采样调用后再编译、验证并反射；不按名字对未解析正文做全局替换，也不在运行时打开系统 SDK 头文件。

本指南描述当前 Windows/DXIL 路径的接口和实现边界。CPU 编译契约与实际 Direct3D12 像素验收分别记录；不据此宣称其他 GPU 后端已通过。

### 仅使用深度或距离

HLSL 仍须声明主 `Texture2D scene`，然后声明 `scene_depth`／`scene_distance` 及对应 Sampler；可以完全不采样主纹理颜色。Lua 只传 `params.scene = canvas3d`，不要传派生字段。即使主颜色被编译器优化掉，也会校验主字段、Canvas3D 类型和本次实际输出目标；用户不得采样当前目标的任何附件。
