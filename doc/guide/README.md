# Selen2D 入门

这个示例由 EXE 内编译的启动配置直接执行 `example/main.lua`。默认只接入部署目录中的 `example/` 和 `output/`，不读取外部启动文件。画面由自定义 HLSL 全屏绘制：WASD 或方向键移动发光圆形，Esc 退出。示例不需要图片、字体或资源包，也不会写入截图或测试结果。

- [开始运行与修改 Lua](getting-started.md)：目录、入口、固定项目根和常用修改。
- [编写 Shader](shader.md)：运行时编译、相对 include、Lua 参数与一号机 HLSL ABI。
- [API 声明](../api/)：`modules/*.lua` 与 `runtime/Runtime.lua` 是编辑器定义，不能 `require`、`dofile` 或作为入口执行。
- [帧调度、时间与错误](runtime.md)：二号机已实现的显式对象提交、时间换算和安全关闭。
- [输入快照与窗口坐标](input-window.md)：二号机输入配置、逻辑坐标和状态查询。
- [File、资源包与编译期启动配置](file-launch.md)：虚拟路径、错误返回、SLPK 工具和私有 `launch.h`。

原生模块由引擎注册为顶层 `Runtime`、`Window`、`Input`、`Resource`、`Graphic`、`Time` 等全局表。示例直接调用这些表，不从 API 声明文件加载实现。

发行包的项目许可证、第三方说明和依赖许可证分别放在本目录的 `LICENSE`、`THIRD_PARTY_NOTICES.txt` 和 `licenses/` 中。请保留这些文件。

启动和 File 已迁入编译期配置与显式 VFS 授权。Shader 示例仍使用 `Solongo.hlsl` 和当前渲染接口；Resource、Graphic、Audio 的二号机接口继续按阶段迁移。文档不把这些后续工作或非 Windows 平台验收描述为已经完成。
