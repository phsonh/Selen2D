# Selen2D 入门

默认 EXE 执行部署根的 `main.lua`，其中用 `File.RunLuaFile("example/main.lua")` 引导 Orbit Garden。默认授权安全部署根路径；运行时不读取外部 JSON 或启动头文件。Orbit Garden 展示菜单、异步加载、对象更新、暂停与返回菜单：Enter 开始，WASD 或方向键移动，P / 空格暂停，游戏中 Esc 返回菜单、菜单中 Esc 退出。随包包含原创图片/铃声和附 OFL 许可的 Noto Sans 字体；普通运行不会写入截图或测试结果。

- [开始运行与修改 Lua](getting-started.md)：目录、入口、固定项目根和常用修改。
- [完整 Lua 示例](example.md)：资源加载、显式 Object 提交、独立 RNG、播放器、Canvas、释放与两轮自动流程。
- [编写 Shader](shader.md)：运行时编译、相对 include、Lua 参数与 Engine.hlsl ABI。
- [API 声明](../api/)：`modules/*.lua` 与 `runtime/Runtime.lua` 是编辑器定义，不能 `require`、`dofile` 或作为入口执行。
- [帧调度、时间与错误](runtime.md)：二号机已实现的显式对象提交、时间换算和安全关闭。
- [输入快照与窗口坐标](input-window.md)：二号机输入配置、逻辑坐标和状态查询。
- [File、资源包与编译期启动配置](file-launch.md)：虚拟路径、JSON 自动编译和标准发行隔离。
- [PyQt6 源码开发工具](tools-gui.md)：项目 JSON、资源包、构建与安全取消；只在开发机安装。
- [调试控制台与日志](debug-console.md)：默认隐藏、按需显示及启动错误诊断。
- [资源加载](resource.md)：命名选项、异步整批发布、进度和资源生命周期。
- [播放器与音频资源](audio.md)：显式播放器、逐帧重播规则、音量和声像。
- [普通文字](text.md)：横排、竖排、测量、换行与轮廓。
- [小数图片区域](fractional-images.md)：逻辑区域与物理像素覆盖，PMA 过滤与自定义 Shader 的局部采样。

原生模块由引擎注册为顶层 `Runtime`、`Window`、`Input`、`Resource`、`Graphic`、`Time` 等全局表。示例直接调用这些表，不从 API 声明文件加载实现。

发行包的项目许可证、第三方说明和依赖许可证分别放在本目录的 `LICENSE`、`THIRD_PARTY_NOTICES.txt` 和 `licenses/` 中。请保留这些文件。

启动和 File 已迁入编译期配置与显式 VFS 授权，Resource 和 Audio 采用二号机选项表、Loader 与显式播放器。Shader 示例使用 `Engine.hlsl`；Graphic 已提供 Canvas 双入口、Mesh 更新与普通文字；整机组合验收及非 Windows 平台验证另行记录。
