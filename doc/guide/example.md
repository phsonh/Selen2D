# Orbit Garden：完整 Lua 示例

运行发行目录的 EXE，按 Enter 进入花园。WASD 或方向键移动金色光点，碰到漂浮的小光点后计分并生成新的光点。P 或空格暂停，再按 P、空格或 Enter 继续。游戏中 Esc 返回菜单，菜单中 Esc 退出；可以反复进入。标题栏同步显示当前操作。

根目录 `main.lua` 通过 `File.RunLuaFile("example/main.lua")` 引导示例，并检查正常读取失败的返回值。示例逻辑仍完整保存在 `example/main.lua`；图形与声音是本项目原创的小素材，文字使用原版 Noto Sans。字体的原作者、固定来源和 SHA-256 在 `example/assets/SOURCE.md`，SIL OFL 1.1 原文在同目录 `OFL.txt`；分发时保留它们。示例运行不需要 Python、SDK、网络或第三方 DLL。

## 一条完整的生命周期

| 状态 | 实际执行的工作 |
|---|---|
| 菜单 | 只保留 UI 字体、白色像素与 UI Canvas；Enter 创建新的 Loader。 |
| 加载 | 同批登记 Image、Sound、Shader 后 Submit；逐帧查询真实进度，整批发布后才进入。 |
| 运行 | 新建独立 RNG、AudioPlayer、场景 Canvas 和 9 个 Object；显式 Commit 激活。 |
| 暂停 | 不调用 UpdateAll 或碰撞；场景 Shader 的时间也冻结；播放器暂停并保留播放位置。 |
| 返回 | Stop/Destroy 播放器、Delete 对象并 Commit、Destroy 场景 Canvas、Unload 三项游戏资源。 |
| 再进入 | 用同样资源名重新加载，创建新的资源代次、播放器和场景；相同种子得到相同初始布局。 |

UI 的两项资源同步加载，使游戏资源准备期间仍能显示文字与真实进度。Loader 没有 Cancel；加载时按 Esc 会记录返回请求，等待本批完成后卸载。失败批次不会部分发布，示例返回菜单显示错误提示。窗口关闭可以在加载期间退出，剩余后台任务由引擎收尾。

普通帧明确调用 `Object.Commit()`、`Object.UpdateAll()`、`Object.CheckCollision(1, 2)`、`Object.Commit()`。碰撞回调只请求 Delete/New，不在回调中重入 Commit。每个 Runtime 回调推进一个固定逻辑步，目标 60 FPS；这是教学示例的调度选择，拖动或卡顿时不会追赶多个逻辑步。`settings.speed` 按每秒 60 步换算；`settings.seed` 控制布局。游戏与配色各持有独立 RNG，从不替换或消耗模块默认生成器。

绘制使用 720×440 的场景 Canvas 和 960×640 的 UI Canvas，再按窗口逻辑尺寸等比缩放并留黑边。`effect.hlsl` 仍是真实运行时编译的自定义 Shader，包含 `Engine.hlsl` 与相对的 `palette.hlsl`；Lua 传入时间、玩家位置和颜色。Object 的 Render 回调再绘制图片。暂停 UI 叠在场景之上，场景本身不推进。Canvas 方法不会改变模块默认目标。

音效通过 `Audio.CreatePlayer()` 返回的对象调用，不设置模块当前播放器。进入、收集和暂停提示使用短铃声；暂停提示先 Play 再 Pause，恢复时 Resume 同一实例。普通退出回调不 Commit，也不绘制；它只请求剩余对象删除、停止音频并释放资源，由宿主继续排空删除回调。

## 可重复的自动流程

在解压后的发行目录运行：

```powershell
.\Selen2D.exe --self-test
```

自动模式只把键盘输入替换为确定的动作序列，调用同一套加载、更新、碰撞、绘制、暂停和退出函数。它完成两轮“菜单→加载→运行→暂停→继续→返回”，每轮真实收集至少一个光点，并检查显式删除回调、播放器/Canvas 失效、资源卸载及默认 RNG 不变。正常返回菜单后调用 Runtime.Quit，ExitFunction 写入 `output/result.json`。同目录的 PNG 保存菜单、加载、运行、暂停和重新进入画面；场景 PNG 可用于逐像素比较暂停冻结和相同种子布局。

只有 `--self-test` 会创建/写入 `output/`。普通交互不写截图或结果。示例选择的路径是 VFS 的 `example/...` 与 `output/...`，不会随启动终端的当前目录改变；标准配置授权整个安全部署根，并不强制其他游戏采用这两个目录。自动截图会同步等待 GPU，自动模式的帧耗时不代表游戏性能；测试结果也不代表真实扬声器听感或其他平台的设备验收。
