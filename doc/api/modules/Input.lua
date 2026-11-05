---@meta

-- Selen2D Input 声明（19 个公开函数）；仅供编辑器，不应执行。
-- 来源：lua_bind/modules/Input、core/modules/Input/{Input,InputNames,InputState,Gamepad}.cpp。
-- 配置 Getter 立即更新，采样处理从下一帧生效。
-- 宿主在每次帧回调前采集一个快照；同帧重复读取不消耗状态。无刚按下/刚松开事件或文本/IME 输入。
-- 所有接口要求已初始化 SDL 主线程服务；参数数量、类型、大小写或未知名称错误抛 Lua 错误。
-- 窗口失焦、隐藏或最小化时键/按钮为 false、轴与滚轮为 0、鼠标位置 nil；不会自动暂停游戏。
-- 键盘采用物理 Scancode，名称严格小写，不受布局字符变换影响。以下完整键名由基线 InputNames.cpp 核对。
---@alias InputKey
---| "0"
---| "1"
---| "2"
---| "3"
---| "4"
---| "5"
---| "6"
---| "7"
---| "8"
---| "9"
---| "a"
---| "ac_back"
---| "ac_bookmarks"
---| "ac_close"
---| "ac_exit"
---| "ac_forward"
---| "ac_home"
---| "ac_new"
---| "ac_open"
---| "ac_print"
---| "ac_properties"
---| "ac_refresh"
---| "ac_save"
---| "ac_search"
---| "ac_stop"
---| "again"
---| "alt_erase"
---| "apostrophe"
---| "application"
---| "b"
---| "backslash"
---| "backspace"
---| "c"
---| "call"
---| "cancel"
---| "caps_lock"
---| "channel_decrement"
---| "channel_increment"
---| "clear"
---| "clear_again"
---| "comma"
---| "copy"
---| "crsel"
---| "currency_subunit"
---| "currency_unit"
---| "cut"
---| "d"
---| "decimal_separator"
---| "delete"
---| "down"
---| "e"
---| "end"
---| "end_call"
---| "enter"
---| "enter2"
---| "equals"
---| "escape"
---| "execute"
---| "exsel"
---| "f"
---| "f1"
---| "f10"
---| "f11"
---| "f12"
---| "f13"
---| "f14"
---| "f15"
---| "f16"
---| "f17"
---| "f18"
---| "f19"
---| "f2"
---| "f20"
---| "f21"
---| "f22"
---| "f23"
---| "f24"
---| "f3"
---| "f4"
---| "f5"
---| "f6"
---| "f7"
---| "f8"
---| "f9"
---| "find"
---| "g"
---| "grave"
---| "h"
---| "help"
---| "home"
---| "i"
---| "insert"
---| "international1"
---| "international2"
---| "international3"
---| "international4"
---| "international5"
---| "international6"
---| "international7"
---| "international8"
---| "international9"
---| "j"
---| "k"
---| "keypad_0"
---| "keypad_00"
---| "keypad_000"
---| "keypad_1"
---| "keypad_2"
---| "keypad_3"
---| "keypad_4"
---| "keypad_5"
---| "keypad_6"
---| "keypad_7"
---| "keypad_8"
---| "keypad_9"
---| "keypad_a"
---| "keypad_ampersand"
---| "keypad_at"
---| "keypad_b"
---| "keypad_backspace"
---| "keypad_binary"
---| "keypad_c"
---| "keypad_clear"
---| "keypad_clear_entry"
---| "keypad_colon"
---| "keypad_comma"
---| "keypad_d"
---| "keypad_decimal"
---| "keypad_divide"
---| "keypad_double_ampersand"
---| "keypad_double_vertical_bar"
---| "keypad_e"
---| "keypad_enter"
---| "keypad_equals"
---| "keypad_equals_as400"
---| "keypad_exclam"
---| "keypad_f"
---| "keypad_greater"
---| "keypad_hash"
---| "keypad_hexadecimal"
---| "keypad_left_brace"
---| "keypad_left_paren"
---| "keypad_less"
---| "keypad_memory_add"
---| "keypad_memory_clear"
---| "keypad_memory_divide"
---| "keypad_memory_multiply"
---| "keypad_memory_recall"
---| "keypad_memory_store"
---| "keypad_memory_subtract"
---| "keypad_minus"
---| "keypad_multiply"
---| "keypad_octal"
---| "keypad_percent"
---| "keypad_period"
---| "keypad_plus"
---| "keypad_plus_minus"
---| "keypad_power"
---| "keypad_right_brace"
---| "keypad_right_paren"
---| "keypad_space"
---| "keypad_tab"
---| "keypad_vertical_bar"
---| "keypad_xor"
---| "l"
---| "lang1"
---| "lang2"
---| "lang3"
---| "lang4"
---| "lang5"
---| "lang6"
---| "lang7"
---| "lang8"
---| "lang9"
---| "left"
---| "left_alt"
---| "left_bracket"
---| "left_ctrl"
---| "left_gui"
---| "left_shift"
---| "m"
---| "media_eject"
---| "media_fast_forward"
---| "media_next_track"
---| "media_pause"
---| "media_play"
---| "media_play_pause"
---| "media_previous_track"
---| "media_record"
---| "media_rewind"
---| "media_select"
---| "media_stop"
---| "menu"
---| "minus"
---| "mode"
---| "mute"
---| "n"
---| "non_us_backslash"
---| "non_us_hash"
---| "num_lock"
---| "o"
---| "oper"
---| "out"
---| "p"
---| "page_down"
---| "page_up"
---| "paste"
---| "pause"
---| "period"
---| "power"
---| "print_screen"
---| "prior"
---| "q"
---| "r"
---| "right"
---| "right_alt"
---| "right_bracket"
---| "right_ctrl"
---| "right_gui"
---| "right_shift"
---| "s"
---| "scroll_lock"
---| "select"
---| "semicolon"
---| "separator"
---| "slash"
---| "sleep"
---| "soft_left"
---| "soft_right"
---| "space"
---| "stop"
---| "sys_req"
---| "t"
---| "tab"
---| "thousands_separator"
---| "u"
---| "undo"
---| "up"
---| "v"
---| "volume_down"
---| "volume_up"
---| "w"
---| "wake"
---| "x"
---| "y"
---| "z"
---@alias InputMouseButton 'left'|'middle'|'right'|'x1'|'x2'
---@alias InputGamepadButton 'a'|'b'|'x'|'y'|'back'|'guide'|'start'|'left_stick'|'right_stick'|'left_shoulder'|'right_shoulder'|'dpad_up'|'dpad_down'|'dpad_left'|'dpad_right'|'misc1'|'misc2'|'misc3'|'misc4'|'misc5'|'misc6'|'right_paddle1'|'left_paddle1'|'right_paddle2'|'left_paddle2'|'touchpad'|'left_trigger'|'right_trigger'|'left_stick_up'|'left_stick_down'|'left_stick_left'|'left_stick_right'|'right_stick_up'|'right_stick_down'|'right_stick_left'|'right_stick_right'
---@alias InputGamepadAxis 'left_x'|'left_y'|'right_x'|'right_y'|'left_trigger'|'right_trigger'

---@class InputModule
Input = {}

---读取本帧物理键持续按住状态；没有焦点为 false，未知键名抛错（不能用于枚举或探测任意名称）。
---初始快照所有键均未按下；边沿和动作映射由 Lua 自行维护。
---@param key InputKey
---@return boolean down
function Input.IsKeyDown(key) end

---设置游戏鼠标输入开关，默认 true，无返回值；不会影响系统窗口交互或系统光标可见性。
---false 立即清空当前鼠标位置/按钮/滚轮；true 立即改变开关，但要等下一次采集才恢复有效状态。
---重新启用不补发禁用期间的滚轮，不恢复已清掉的同帧快照。
---@param enabled boolean
function Input.SetMouseEnabled(enabled) end

---读取开关（默认 true），与焦点、光标是否在客户区无关；读取不会修改快照。
---@return boolean enabled
function Input.IsMouseEnabled() end

---有效时返回客户区中心原点的逻辑坐标，X 向右/Y 向上，可有小数。
---鼠标禁用、失焦、隐藏/最小化、离开客户区或尚无有效采样时只返回单个 nil。
---与 Window.GetSize 共用本帧已确认尺寸；(u,v) 转为 (u-W/2,H/2-v)，不使用 GPU 像素。
---@return number|nil x
---@return number? y 仅 x 非 nil 时存在。
function Input.GetMousePosition() end

---返回本帧累计滚轮增量，横向右为正、纵向上为正，允许小数；无滚动为 0,0。
---读取不清零，下一次采集重置；鼠标位置无效/禁用时为 0,0，不补发失效期间历史输入。
---@return number wheelX
---@return number wheelY
function Input.GetMouseWheel() end

---读取本帧鼠标按钮持续状态；鼠标位置无效或禁用时全部 false。仅接受五个已列出的名称。
---@param button InputMouseButton
---@return boolean down
function Input.IsMouseButtonDown(button) end

---设置游戏手柄输入开关，默认 true；只接受 boolean，不关闭物理设备。
---false 立即清空轴、按钮及迟滞；同帧 true 不恢复旧输入，下一次采集才重新发布。
---@param enabled boolean
function Input.SetGamepadEnabled(enabled) end

---返回配置开关；失焦、最小化或断开设备不修改此配置。
---@return boolean enabled
function Input.IsGamepadEnabled() end

---返回当前逻辑手柄的连接快照；与启用、焦点、最小化和按钮状态独立。
---查询不打开设备或泵事件；重映射的中立输入帧不伪装成设备断开。
---@return boolean connected
function Input.IsGamepadConnected() end

---设置两个摇杆的径向死区，默认 0.2,0.2；有限 number 且 0<=d<1。
---省略 right 则等于 left；显式传入时必须为 number；整组验证成功后原子更新。
---配置立即可读；下次采集按新值重映射并清除有关方向迟滞，不追改本帧轴/按钮。
---@param left number
---@param right? number
function Input.SetGamepadDeadzone(left, right) end

---@return number left
---@return number right
function Input.GetGamepadDeadzone() end

---设置两扳机死区，默认 0.05,0.05；范围及原子性同摇杆死区。
---归一化值 r<=d 为 0，否则 (r-d)/(1-d)；轴及数字按钮共用此结果。
---@param left number
---@param right? number
function Input.SetGamepadTriggerDeadzone(left, right) end

---@return number left
---@return number right
function Input.GetGamepadTriggerDeadzone() end

---设置八个摇杆方向按钮共用的阈值，默认 0.5,0.4；有限 0<=release<press<=1。
---省略 release 时为 press*0.8；>=press 激活，<=release 释放，中间保持。
---改变配置只在下一采集清除旧迟滞并应用；各方向独立，可同时激活两个斜向分量。
---@param press number
---@param release? number
function Input.SetGamepadStickButtonThreshold(press, release) end

---@return number press
---@return number release
function Input.GetGamepadStickButtonThreshold() end

---设置两扳机数字按钮共用的阈值，默认 0.5,0.4；范围和时机同摇杆按钮阈值。
---按扳机死区处理后的模拟值判定；数字按钮不改变连续轴值。
---@param press number
---@param release? number
function Input.SetGamepadTriggerThreshold(press, release) end

---@return number press
---@return number release
function Input.GetGamepadTriggerThreshold() end

---读取唯一逻辑手柄的标准位置按钮；没有设备/失焦/设备不支持该按钮时 false。
---a/b/x/y 为标准位置语义，非所有品牌印字；left_stick/right_stick 始终是实体 L3/R3。
---方向虚拟按钮使用带方向后缀的八个名字；未启用时仍校验名字，未知名称抛错。
---断开/替换/重映射、失焦、禁用会清除旧按钮迟滞，不混合多个设备。
---@param button InputGamepadButton
---@return boolean down
function Input.IsGamepadButtonDown(button) end

---读取标准模拟轴；摇杆 [-1,1]、X 向右/Y 向上，扳机 [0,1]；无设备/失焦返回 0。
---使用当前配置的径向死区/扳机死区；摇杆向量裁剪到单位圆，死区外线性重映射。
---同帧重复读取保持一致；数值配置修改下一次采集生效，禁用/设备失效可立即中立。
---@param axis InputGamepadAxis
---@return number value
function Input.GetGamepadAxis(axis) end
