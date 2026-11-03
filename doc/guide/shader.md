# 示例 Shader

`example/main.lua` 在启动时调用：

```lua
Resource.LoadShader('example.effect', 'example/effect.hlsl')
```

这是同步资源加载：读取源码与 include，进行运行时编译、反射及 GPU 资源准备，成功后才发布资源名。编译失败会报告资源诊断，示例不会换用另一套画面掩盖错误。运行时无需调用 `dxc.exe`，也不读取 SDK、tools 或 Python；应使用完整构建产物随附的编译支持。

Shader 开头包含两个文件：

```hlsl
#include "Solongo.hlsl"
#include "palette.hlsl"
```

`Solongo.hlsl` 是引擎提供的内置公共声明，不需要在游戏目录另放一个同名文件。名字沿用一号机兼容 ABI；产品改名不改变这个 include，也不改变 `SolongoTargetSize` 等内置字段。不要自行重定义 `VSInput`、`PSInput` 或占用 `Solongo` 前缀。

`palette.hlsl` 是实际的示例文件，通过 VFS 从 Shader 所在目录解析。它提供 `AnimatedAccent` 和 `SoftDisk` 两个辅助函数。修改该文件、保存并重启程序，就会在下一次资源加载时重新编译；加载后的 Shader 不自动监视文件变化。

一号机的像素入口签名为：

```hlsl
float4 PSMain(PSInput input) : SV_Target0
```

示例省略 `VSMain`，使用引擎默认顶点处理。Lua 帧回调中的 `Graphic.Render('example.effect', params)` 不传 Mesh，因此覆盖整个当前目标；每帧开始时默认目标是窗口。全屏 `input.uv` 左上为 `(0,0)`、右下为 `(1,1)`。Shader 利用 `SolongoTargetSize` 修正宽高比，把 `center` 定义为居中、Y 向上的坐标，单位为窗口高度，因此窗口变宽不会把圆拉成椭圆。

用户数值放在唯一的 `cbuffer Params` 中，不手写 GPU `register` 绑定。Lua table 的字段名大小写必须与 HLSL 声明一致：

| HLSL 字段 | 类型 | Lua 示例 | 用途 |
|---|---|---|---|
| `elapsed` | `float` | `1.25` | 动画时间，秒 |
| `center` | `float2` | `{0.1, -0.05}` | 圆形位置 |
| `radius` | `float` | `0.12` | 圆形半径 |
| `accentColor` | `float3` | `{0.25, 0.78, 1.0}` | 主色 RGB |
| `backgroundColor` | `float3` | `{0.018, 0.028, 0.055}` | 背景 RGB |

这些值经过实际编译反射后按字段布局上传。向量必须是恰好对应数量的连续数组，参数 table 和向量都不能带元表；不能把 ARGB 整数当作 `float3/float4`。未知字段、错误类型或缺少实际使用的字段会报错。`SolongoTargetSize` 和 `SolongoTargetInvSize` 由引擎提供，不要放进 Lua params。

`Graphic.Render` 在调用时复制参数，本次提交之后修改 table 只影响下一次提交。每次提交都要提供需要的字段，没有“沿用上一帧参数”的隐式状态。示例直接修改 `params.center`、`params.elapsed`，然后提交一张完整画面。

像素入口返回 Straight RGBA；引擎最终预乘一次并以既定混合方式提交。示例返回 Alpha `1.0`，绘制不透明背景和圆形，不依赖图片或额外的字体接口。

本指南描述示例代码和接口契约，不作为 DXC 静态部署验收记录；部署验收结果以本次构建的最终验证为准。
