# 输入快照与窗口坐标

Input 提供 19 个函数，Window 提供 17 个函数。完整参数和名称见 [Input 声明](../api/modules/Input.lua) 和 [Window 声明](../api/modules/Window.lua)。输入在每次帧回调前采集，同帧重复读取不会消耗按键或滚轮；按下、松开边沿由 Lua 比较相邻帧计算。

窗口尺寸和鼠标使用同一帧已经确认的客户区逻辑尺寸。`Window.GetSize()` 返回 `W,H`；有效鼠标以窗口中心为原点，X 右正、Y 上正。例如窗口为 `800×600`，左上坐标 `(100,50)` 返回 `(-300,250)`。鼠标离开客户区、失焦、最小化或禁用时，`Input.GetMousePosition()` 只返回一个 `nil`。不要给 Lua 坐标再乘除 DPI 比例。窗口像素尺寸由图形后端管理。

`Window.SetSize()` 请求改变客户区，操作系统可能延迟应用。帧回调内改变尺寸不会追改当前输入快照，下一轮采集使用已确认的新尺寸。`Window.IsMinimized()` 只表示已收到系统最小化确认，隐藏、失焦或暂时无法呈现均不能替代这个状态。它不负责暂停游戏；`Window.IsFullscreen()` 同样不会把隐藏窗口的待应用全屏请求报告为成功。

## 查询显示器桌面尺寸

2.3.13 新增 `Window.GetScreenSize()`，严格零参数，返回 `width,height` 两个正整数，表示游戏窗口关联显示器的桌面模式像素尺寸。关联显示器由 SDL 确定，普通窗口通常按中心所在显示器选择，全屏使用对应显示器；不会固定读取主屏，也不合并所有显示器的大小。复制显示遵循操作系统提供的桌面模式，不能用此接口分别选择复制组中的面板。

```lua
-- Lua 入口执行前已有隐藏窗口，此查询不会显示窗口。
local screenWidth, screenHeight = Window.GetScreenSize()
-- 示例：计算 4:3 标准画布在该显示器上的全屏倍率。
local scale = math.min(screenWidth / 1280, screenHeight / 960)
```

这里查询的是系统当前配置的桌面模式，不是显示器硬件最大分辨率、排除任务栏后的工作区或窗口客户区。SDL desktop mode 的宽高乘 `pixel_density` 后四舍五入为实际像素；这与 Windows 的内容/UI 缩放倍率不同，应用不应再乘系统缩放。`Window.GetSize()` 仍返回与鼠标和窗口绘制坐标一致的客户区尺寸，两者职责不同。

每次调用读取 SDL 当前已知状态，不缓存启动结果、不泵事件、不额外同步等待；移屏或桌面模式变化在宿主处理相关系统事件后反映。启动时可以查询一次，进入全屏或画面比例变化时重新计算倍率；确认全屏生效后的实际输出区域仍应按窗口状态处理。进入/退出全屏继续使用 `Window.SetFullscreen()`，此查询本身不调整窗口、焦点、全屏状态或帧快照。

调用必须在已初始化窗口所属主线程执行；窗口无效、显示器或桌面模式查询失败，以及像素密度/尺寸无效或超过原生整数范围时抛错，不用 `0,0` 或无关显示器的尺寸掩盖错误。可在 Lua 入口、帧回调及窗口仍有效的退出回调中调用。

## 还原窗口后调整尺寸

2.3.7 增加 `Window.Restore()` 和 `Window.IsMaximized()`。前者严格无参数、无返回值，沿用 Window 的主线程、服务有效性检查及平台失败抛错；后者严格无参数，返回一个已收到窗口事件确认的 boolean。不会把还原请求提前报告成完成，不泵事件，也不额外调用 SDL_SyncWindow 等待完成确认。平台调用仍可能耗时，SDL_VIDEO_SYNC_WINDOW_OPERATIONS 环境选项也可能要求底层同步；引擎不启用该选项。最大化与全屏分别查询。

修改最大化窗口的尺寸前，先请求还原，在后续帧确认非全屏、非最小化、非最大化后再调用 SetSize。下面的 `updateResize` 应放进应用已有的帧回调；它保留有界超时，不在同一回调里忙等事件：

```lua
local pendingResize
local function requestResize(width, height)
    if Window.IsFullscreen() then return false, "请先退出全屏并等待确认" end
    Window.Restore()
    pendingResize = { width, height, deadline = Time.GetElapsedMilliseconds() + 3000 }
    return true -- 只代表发起请求。
end
local function updateResize()
    local pending = pendingResize
    if not pending then return end
    if not Window.IsFullscreen() and not Window.IsMinimized() and not Window.IsMaximized() then
        pendingResize = nil
        Window.SetSize(pending[1], pending[2]) -- 实际尺寸仍在后续帧确认。
    elseif Time.GetElapsedMilliseconds() >= pending.deadline then
        pendingResize = nil
        Debug.Warn("窗口尚未还原；取消本次尺寸请求")
    end
end
```

SDL/窗口系统可以延迟或拒绝状态变化。最小化窗口可能恢复到原最大化状态，收到 RESTORED 后还可能紧跟 MAXIMIZED；应以后续查询为准，必要时由应用在有界流程中再次请求还原，不能承诺一次 Restore 必然成为普通窗口。

- 普通窗口重复 Restore 不调整客户区尺寸；不额外调用聚焦、显示或退出全屏操作。实际还原调用产生的激活/显示由窗口系统处理，不承诺焦点绝对不变。
- 隐藏启动阶段只清除 SDL 待应用的最小化/最大化请求，不显示窗口，也不取消待应用的全屏。宿主仍在入口完成后显示。
- 全屏期间 Restore 不直接退出全屏，可能改变退出全屏后的窗口化状态；全屏且最小化时也可能保持不可见。需要普通窗口时，先显式 SetFullscreen(false) 并等待 IsFullscreen() 确认，再处理还原。
- 状态查询只消费宿主已处理的确认事件。Restore 不改 VSync、目标FPS、输入或离屏Canvas；帧内 GetSize 与鼠标仍共用原来的尺寸快照。

## 输入设置

鼠标和手柄默认启用。关闭开关立即使对应查询中立，同帧再次启用也不恢复已清空的输入；新输入须等下一次采集。光标显示由 `Window.SetCursorVisible()` 控制，与游戏鼠标开关独立。

```lua
Input.SetGamepadDeadzone(0.2)              -- 左右摇杆径向死区
Input.SetGamepadTriggerDeadzone(0.05)      -- 左右扳机死区
Input.SetGamepadStickButtonThreshold(0.5, 0.4)
Input.SetGamepadTriggerThreshold(0.5, 0.4)
local left, right = Input.GetGamepadDeadzone()
```

这些也是默认值。两个死区 Setter 可分别指定左右值；省略右值则与左值相同。两个阈值 Setter 省略释放阈值时使用按下阈值的 `0.8` 倍。死区要求有限数值 `0 <= d < 1`；阈值要求有限数值 `0 <= release < press <= 1`。非法参数整组拒绝，配置不发生部分更新。Getter 立即反映合法修改，轴和数字按钮从下一次采集应用新配置。

摇杆先归一化并裁剪到单位圆，再按径向死区重映射；扳机死区外也线性重映射到满幅 `1`。数字按钮使用同一过滤后的值，超过按下阈值激活，下降至释放阈值松开，中间保持。`left_stick`/`right_stick` 是实体 L3/R3；方向查询分别使用 `left_stick_up`、`left_stick_down`、`left_stick_left`、`left_stick_right` 和对应 `right_stick_*`。斜向允许同时激活两方向。

`Input.IsGamepadConnected()` 查询一个逻辑手柄的连接快照；禁用、失焦和最小化不伪装成断开。设备替换、重映射或输入失效会清除旧迟滞。没有手柄时仍可配置参数，未知名称仍报错；模块不自动存盘，也不提供设备槽位、按下事件或动作映射。

这些说明描述已实现的状态与接口契约。独立 CPU 夹具覆盖状态变换和参数验证；真实硬件热插拔、跨显示器 DPI、系统最小化/全屏及 Graphic 投影的组合结果需要原生集成验收，不能由 CPU 结果替代。
