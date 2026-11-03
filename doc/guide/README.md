# Selen2D 入门

这个示例从 EXE 同目录的 `launch.lua` 启动，再执行 `example/main.lua`。画面由自定义 HLSL 全屏绘制：WASD 或方向键移动发光圆形，Esc 退出。示例不需要图片、字体或资源包，也不会写入截图或测试结果。

- [开始运行与修改 Lua](getting-started.md)：目录、入口、固定项目根和常用修改。
- [编写 Shader](shader.md)：运行时编译、相对 include、Lua 参数与一号机 HLSL ABI。
- [API 声明](../api/)：`modules/*.lua` 与 `runtime/Runtime.lua` 是编辑器定义，不能 `require`、`dofile` 或作为入口执行。

原生模块由引擎注册为顶层 `Runtime`、`Window`、`Input`、`Resource`、`Graphic`、`Time` 等全局表。示例直接调用这些表，不从 API 声明文件加载实现。

发行包的项目许可证、第三方说明和依赖许可证分别放在本目录的 `LICENSE`、`THIRD_PARTY_NOTICES.txt` 和 `licenses/` 中。请保留这些文件。

这里记录的是一号机已有接口与启动壳，不引入 `launch.h`、授权配置或加密配置。
