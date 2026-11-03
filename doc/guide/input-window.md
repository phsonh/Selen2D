# 输入快照与窗口坐标

Input 提供 19 个函数，Window 提供 14 个函数。完整参数和名称见 [Input 声明](../api/modules/Input.lua) 和 [Window 声明](../api/modules/Window.lua)。输入在每次帧回调前采集，同帧重复读取不会消耗按键或滚轮；按下、松开边沿由 Lua 比较相邻帧计算。

窗口尺寸和鼠标使用同一帧已经确认的客户区逻辑尺寸。`Window.GetSize()` 返回 `W,H`；有效鼠标以窗口中心为原点，X 右正、Y 上正。例如窗口为 `800×600`，左上坐标 `(100,50)` 返回 `(-300,250)`。鼠标离开客户区、失焦、最小化或禁用时，`Input.GetMousePosition()` 只返回一个 `nil`。不要给 Lua 坐标再乘除 DPI 比例。窗口像素尺寸由图形后端管理。

`Window.SetSize()` 请求改变客户区，操作系统可能延迟应用。帧回调内改变尺寸不会追改当前输入快照，下一轮采集使用已确认的新尺寸。`Window.IsMinimized()` 只表示已收到系统最小化确认，隐藏、失焦或暂时无法呈现均不能替代这个状态。它不负责暂停游戏；`Window.IsFullscreen()` 同样不会把隐藏窗口的待应用全屏请求报告为成功。

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
