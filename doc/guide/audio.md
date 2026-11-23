# 播放器与音频资源

先由 Resource 加载 Sound，再创建用途由脚本决定的播放器。加载时明确指定 `"memory"`（完整解码到 PCM）或 `"stream"`（流式解码），两种模式默认均单次播放；循环由独立的 `loop` 选项决定。创建播放器不会自动选中；引擎启动时当前播放器为 `nil`。

```lua
Resource.LoadSound("hit", "example/audio/hit.wav")
Resource.LoadSound("theme", "example/audio/theme.ogg", { loadmode = "stream", loop = true, loopStart = 3.5 })

local effects = Audio.CreatePlayer()
local music = Audio.CreatePlayer()
effects:SetVolume(0.8)
music:SetVolume(0.6)
Audio.SetPlayer(effects)
music:Play("theme")
Audio.Play("hit")
```

`Resource.LoadSound(name,path,options?)` 和 `loader:LoadSound(...)` 使用相同参数。`options.loadmode` 只接受 `"memory"` 或 `"stream"`，省略/nil 默认 `"memory"`。options 省略、nil 或空表均不循环；`loop` 必须为布尔值，默认 false。`loop=true` 时默认整段循环，首次从 0 播放至 loopEnd，之后重复 [loopStart,loopEnd)；也可指定以秒表示的端点，要求 `0 <= loopStart < loopEnd <= 源时长`。未指定的起点为 0、终点为源时长。未启用 loop 却指定任一端点会报错。memory 可以循环，stream 也可以单次播放。

`options.volume` 指定初始资源音量，为有限数 `[0,1]`，省略或 nil 默认 `1`。它与 loadmode、loop 独立，加载完成时已经生效，无需额外调用 `SetVolume`。例如 `Resource.LoadSound("hit", "example/hit.wav", {volume=0.6})`。Loader 在登记时复制音量值，之后修改 options 不会改变已登记的请求；非法音量立即抛错且不占用名字。

旧 `LoadMusic` 及 Music 资源类型已移除。旧四参数 `LoadSound(name,path,loadmode,options)` 把 loadmode 移进 options；第三参数传字符串现在报错。流式循环资源写成 `LoadSound(name,path,{loadmode="stream",loop=true,...})`。双参数使用默认 memory，默认不循环。两种模式的 `Resource.GetType(name)` 均为 `"Sound"`。播放器、同帧去重、音量和暂停接口不变，完整加载声明见 [Resource API](../api/modules/Resource.lua)。

路径必须由编译期启动配置授权。模块入口作用于当前播放器；对象入口只操作该对象，不改变选择。未选择播放器时，播放、控制和查询都会报错。`Audio.SetPlayer(nil)` 仅取消选择，正在播放的声音继续推进。

一个播放器可以同时播放多个不同资源。同一播放器对同一资源代次只保留一个实例：每个 Runtime 帧的第一次有效 `Play` 从 0 秒重播，重复调用合并；不同播放器的游标独立。入口脚本为第 0 帧。`Play → Pause → Play` 在同帧保持暂停；`Stop → Play` 明确允许同帧重播。不要在每个帧回调中无条件播放循环音乐。

`Stop(name?)`、`Pause(name?)`、`Resume(name?)` 省略名称或传 `nil` 时作用于此播放器全部实例。指定名称时仍验证它是已就绪的 Sound；有效资源没有实例时为空操作。暂停保留位置，恢复不重新播放已经播放中的实例。

播放器音量为有限数 `[0,1]`，声像为有限数 `[-1,1]`；非法设置报错并保持原值。基本增益为播放器音量乘 `Resource.GetVolume(name)`。声像为左右平衡：正值衰减左侧，负值衰减右侧，0 保持原立体声；单声道先复制到左右声道。设置影响当前及后续声音，暂停者恢复时采用新设置。零音量仍然算正在播放。

`GetPlaying()` 返回独立的、按 UTF-8 字节排序的名称数组；`IsPlaying(name)` 返回当前是否逻辑播放。暂停、停止和自然结束者不包含，静音者包含。成功 Play 后立即可查询为 true；设备缓冲可能延迟实际可听变化。

`player:Destroy()` 停止该播放器全部实例并撤销其句柄；重复调用无害，之后其他方法报错。销毁当前播放器会使当前选择变为 nil，不会卸载资源。未选中的不可达播放器由 GC 兜底销毁；当前选择保活同一 Lua 对象。`Resource.Unload(name)` 停止所有播放器中的对应资源代次，并清除去重记录；同名重新加载得到新代次。

音频混音与流式补给不依赖窗口 Present、Object 更新或下一帧。流式资源保留首次获准打开的来源，循环和新播放器不会重新按路径打开同名替换文件。退出时先停止播放器并排空解码消费者，再释放资源和文件系统。

模块提供 13 个函数，播放器提供 11 个方法，完整声明见 [Audio API](../api/modules/Audio.lua)。控制和 Setter 成功返回零个值；没有单次播放句柄、Seek、固定音乐槽或分类全局音量。当前验收采用原生 CPU 与真实 SDL dummy 最终混音采样，未据此宣称真实音频硬件听感或窗口最小化人工验收完成。
