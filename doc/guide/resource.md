# 资源加载

资源使用统一名称空间，Image、Sound、Music、Font、Shader 不可使用相同名字。路径相对编译期授权的 VFS 根，不依赖进程工作目录。

```lua
Resource.LoadImage("player", "example/player.png", {
    u = 0.5, v = 8, w = 32, h = 32,
    centerX = 16, hscale = 0.5, vscale = 0.75,
})
Resource.LoadMusic("stage", "example/stage.ogg", { loopStart = 12.5 })
```

Image/Music 第三参数可省略、为 nil 或空表。其余加载函数只接受名称和路径。选项只读表自身字段，不执行元方法、不补写默认值；未知键、数字键、数字字符串、旧位置参数和 scale 别名均报错。

图片 u/v 默认 0，w/h 独立默认源宽-u、源高-v。中心各轴独立默认最终切片的一半。保留小数裁剪；缩放不改变切片尺寸、中心或 UV。hscale 默认 1，vscale 默认最终 hscale。mipmap 默认 false，采样器默认 linear+clamp。

音乐默认整段循环，也可只给起点或终点；首次从 0 播放，之后重复 [loopStart,loopEnd)。音效完整解码，音乐流式解码。加载本身不播放，参见[音频指南](audio.md)。

## 非阻塞加载

```lua
local loader = Resource.CreateLoader()
local options = { hscale = 0.5 }
loader:LoadImage("a", "example/a.png", options)
options.hscale = 1
loader:LoadImage("b", "example/b.png", options)
loader:LoadSound("shot", "example/shot.wav")
loader:Submit()

-- 在现有帧回调中调用，继续绘制加载画面。
local function updateLoading()
    local progress = loader:GetProgress()
    if loader:IsFinished() then
        -- 全批资源已公开；a 的缩放为 0.5，b 为 1。
        return true, progress
    end
    return false, progress
end
```

可跨帧登记，每次登记立即复制选项快照。Submit 接受后封存并预留名称。原生系统继续读取、解码、编译、构建 Atlas 和上传；查询不泵任务、不等待 GPU 或窗口 Present。不查询也不会阻止维护清理。

进度是 C/(N+1)：N 项请求中 C 项真正准备完成，最后一项代表整批发布。未发布始终小于 1，成功公开才是 1。空批次 Submit 前为 0，接受后立即为 1。这不是耗时或字节百分比。

IsFinished/GetProgress 遇到失败终态都会重复抛出同一错误，含名称、路径和原因。可以用 pcall 捕获，不要无限等待失败任务。任何一项失败，全批不公开并解除预留；重试需要新 Loader。提交时名字冲突不接受该次提交，修复外部冲突后可重试原 Loader。

## 生命周期和元数据

Resource.GetType(name) 仅看已发布资源，返回五种类型字符串或 nil；pending 名称也为 nil。其他 Getter、Setter 和 Unload 对未知、类型错误或未就绪资源报错。

```lua
local w, h = Resource.GetImageSize("player")
Resource.SetImageScale("player", -0.5, 0.75)
Resource.SetImageCenter("player", w / 2, h / 2)
Resource.SetImageSampler("player", "linear+clamp")
Resource.Unload("player")
```

元数据只影响后续绘制。已提交命令保存原资源、元数据和采样状态；卸载立即撤销名称，GPU 使用的旧代次到安全完成才释放。同名重载不替换旧命令。资源不因 Loader 的 Lua GC 而卸载。

Shader 编译、include 与公开 ABI 以[Shader 指南](shader.md)为准；资源接口不另定义 ABI。图形与文字能力以当前已实现 API 为准。
