# 小数裁剪与当前 Shader 边界

`Resource.LoadImage` 与 Loader 的 options 使用源图像素坐标。`u/v`、`w/h` 可以是有限小数，分别省略时取 0 或源图剩余宽高；小于一像素的正宽高也保留。`Resource.GetImageSize` 返回逻辑裁剪宽高，不返回包含该区域的整数像素覆盖区。

原生 `RenderImage`、`RenderRect`、`Render4V` 和 `RenderRibbon` 共享逻辑区域。几何、中心、固有双轴缩放，以及局部 UV 使用逻辑宽高。内部仍按整数像素覆盖区存储、复制和分配纹理；其整数边界不替代逻辑边界。

point 采样使用映射到逻辑区域后的源像素；linear 保持源像素之间的插值。clamp 将坐标限制到本 Image；wrap 周期为逻辑宽高，包含小数部分，不能使用整数覆盖区作为周期。linear 的重复接缝对周期延拓的裁剪像素场进行一像素宽过滤，整数裁剪时与普通双线性 repeat 一致。小于一像素的周期通过解析积分处理，不按周期数循环。Mip 的逻辑矩形按各级实际尺寸映射，LOD 使用寻址前的逻辑 UV 导数。

阶段 D 的明确未完成项：自定义 Shader 绑定具有小数裁剪边界的 Image 会报告 `Custom Shader Image sampling currently requires integer crop bounds`。已有整数 Image 绑定保持可用。原始 `Texture2D.Sample` 使用硬件纹理视图，不能只靠复制整数覆盖区保留小数裁剪和 wrap；完整适配需要后续 Shader 编译与采样元数据改造。此错误不能作为最终阶段 E/F 的完成状态，也不意味着改成整数裁剪等价。

本说明不声明 PMA/透明边缘、公共 HLSL ABI 升级或硬件验收已经完成。实际像素参考和运行结果记录在开发测试中。
