# 第三方源码与构建适配

通常保留上游源码原样，Solongo 的构建适配放在本目录的 CMake 文件或 src/ 中。
已确认的上游缺陷采用有记录的最小局部补丁，保留原许可证与来源，不伪称未经修改。

- SDL3：从 Coral2D 工作区复制，提交 c5f07f09902646a37a73c0f3b32102902a99b69f；许可证 SDL/LICENSE.txt。
  本地补丁：`SDL/src/gpu/SDL_gpu.c` 的 `SDL_GPU_BlitCommon` 将源 mip 的宽高分母钳制为至少1，
  修正非方形完整 mip 链短边右移到0后的除零；`SDL/src/gpu/d3d12/SDL_gpu_d3d12.c` 的
  `D3D12_BeginRenderPass` 同样钳制color/depth附件的mip宽高，防止默认scissor变成空区域。
  修改标注在源文件；没有改 Coral2D 或旧引擎副本。
  修前真实 D3D12 13×7 的第3级RGBA读回为0（预期R=32），由 UploadD3D12 覆盖各级、长条两方向及透明RGB。
- LuaJIT：从 Coral2D 工作区复制，提交 1ee778a4e37122d8ca7d5733c590a47dafd6b15c；许可证 LuaJIT/COPYRIGHT。
- nlohmann/json 3.12.0：从旧引擎已下载的官方 include.zip 缓存复制 single_include/nlohmann/json.hpp。
  原始包 SHA-256 B8CB0EF2DD7F57F18933997C9934BB1FA962594F701CD5A8D3C2C80541559372；
  单头文件 SHA-256 AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63；许可证 nlohmann_json/LICENSE.MIT。
- libogg 1.3.6、libvorbis 1.3.7：复用旧工程缓存的 Xiph 源码，许可证分别为
  libogg/COPYING、libvorbis/COPYING。Solongo 的构建适配在 cmake/audio/，不改上游源码。
  生产音频使用 Vorbis 解码；测试为生成自足 Ogg fixture 单独编入 vorbisenc，不增加发行依赖。
- dr_wav 0.14.4：增量 WAV 解码，许可证 dr_wav/LICENSE；只在 Decoder.cpp 中编译实现。
- stb_image 2.30：PNG-only CPU解码；来源、许可证和实际调色板索引范围局部补丁见 stb_image/README.md。

不提交第三方构建二进制；LuaJIT 在 build 内的副本中编译，SDL 使用独立构建目录。
