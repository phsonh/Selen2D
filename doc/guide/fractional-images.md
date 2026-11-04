# 小数裁剪与 Shader 采样

`Resource.LoadImage` 与 Loader 的 options 使用源图像素坐标。`u/v`、`w/h` 可以是有限小数，分别省略时取 0 或源图剩余宽高；小于一像素的正宽高也保留。`Resource.GetImageSize` 返回逻辑裁剪宽高，不返回包含该区域的整数像素覆盖区。

原生 `RenderImage`、`RenderRect`、`Render4V` 和 `RenderRibbon` 共享逻辑区域。几何、中心、固有双轴缩放，以及局部 UV 使用逻辑宽高。内部仍按整数像素覆盖区存储、复制和分配纹理；其整数边界不替代逻辑边界。

point 采样使用映射到逻辑区域后的源像素；linear 保持源像素之间的插值。clamp 将坐标限制到本 Image；wrap 周期为逻辑宽高，包含小数部分，不能使用整数覆盖区作为周期。linear 的重复接缝对周期延拓的裁剪像素场进行一像素宽过滤，整数裁剪时与普通双线性 repeat 一致。小于一像素的周期通过解析积分处理，不按周期数循环。Mip 的逻辑矩形按各级实际尺寸映射，LOD 使用寻址前的逻辑 UV 导数。

自定义 Shader 的已支持颜色 `Sample`、`SampleLevel` 与原生图元共用局部采样算法；小数裁剪不再因只能创建整数硬件视图而拒绝。内部视图复制整数覆盖区，每条 Draw 单独封存逻辑矩形、过滤、寻址与 mip 元数据，资源采样器变更不会追改旧命令。详见 [Shader 指南](shader.md) 的支持形式与诊断边界。

Image 和 Canvas 均先对预乘工作数据过滤，再返回 Straight 颜色；本 Image 的中心或固有 scale 不参与纹理 UV 映射。当前 RGBA8 工作数据的低 Alpha 量化、sRGB 编码分量运算与 PNG 解释同样以 Shader 指南为准。

本说明不把编译成功等同于像素正确；实际参考、源与目标格式、允许量化误差及 Direct3D12 运行结果记录在开发测试中。
