---@meta

-- Selen2D 二号机 Audio 契约；编辑器定义，不参与运行。
-- 初始化没有播放器或默认选择。模块函数操作当前播放器；对象方法只操作 self，
-- 成功或失败都不改变当前选择。模块控制/查询在未选择播放器时抛状态错误。
-- 全部接口严格检查参数数量和类型。控制与 Setter（包括 SetPlayer）成功零返回；
-- CreatePlayer 与 Getter 返回下面声明的单值。不存在分类播放、固定分类音量、
-- 单次播放句柄、Seek、设备或 FFT 接口。
-- name 是已发布且就绪的 Sound/Music 资源名，不是路径；非空、至多 512 字节、无 NUL。
-- 未知/未发布/非音频资源始终抛错，包括无实例时的控制与 IsPlaying。
-- 播放不依赖 Present、Object 更新或窗口状态；失焦/最小化不隐式暂停。
-- 流式读取使用 File/VFS 授权且保留身份的来源。实际可听控制受设备缓冲延迟影响；
-- 此声明不代表已验证真实音响、各平台设备或最小化行为。

---@class AudioModule
Audio = {}

---创建空播放器，volume=1、pan=0；第一次及后续创建都不自动选择。
---@return AudioPlayer player
function Audio.CreatePlayer() end

---选择同一 userdata 或传 nil 取消选择，成功零返回；错误类型/已销毁句柄保持旧选择。
---当前选择强引用播放器；切换/取消不停止声音、不搬迁实例、不清除同帧去重记录。
---@param player AudioPlayer|nil
function Audio.SetPlayer(player) end

---返回当前选中的同一 userdata；未选择返回单个 nil。
---@return AudioPlayer|nil player
function Audio.GetPlayer() end

---在当前播放器中从头播放；Sound 单次，Music 经过前奏后按资源循环区间循环。
---同一播放器/资源代次/Runtime 帧只接受第一次成功 Play，模块与对象入口共享记录。
---下一有效帧重播会截断本播放器该资源的旧实例；其他资源/播放器不受影响。
---@param name string
function Audio.Play(name) end

---停止指定资源或全部实例，含暂停者；清除相应去重记录，允许同帧 Stop 后重新 Play。
---省略/nil 表示全部；有效资源没有实例为空操作，不卸载资源。
---@param name? string
function Audio.Stop(name) end

---暂停指定或全部正在播放的实例，保留位置/循环状态；不锁定后续其他 Play。
---省略/nil 表示全部；不清除去重记录，同帧 Play→Pause→Play 仍保持暂停。
---@param name? string
function Audio.Pause(name) end

---恢复指定或全部暂停实例；已有播放不重启、没有实例不新建，不清除去重记录。
---@param name? string
function Audio.Resume(name) end

---设置当前播放器当前/后续实例的音量；有限 [0,1]，默认 1，非法值报错且保持原值。
---基本增益为播放器音量乘 Resource 单资源音量；设 0 不暂停或改变 IsPlaying。
---@param volume number
function Audio.SetVolume(volume) end

---返回当前播放器自身音量，不乘资源音量，不读取设备实测结果。
---@return number volume
function Audio.GetVolume() end

---设置左右平衡，有限 [-1,1]，默认 0；非法值报错且保持原值。
---pan>=0 左/右增益为 (1-pan,1)，pan<0 为 (1,1+pan)；0 保持输入立体声平衡。
---影响当前/后续实例，不重播、不改位置、不清除去重记录。
---@param pan number
function Audio.SetPan(pan) end

---返回当前播放器自身声像。
---@return number pan
function Audio.GetPan() end

---返回独立连续 1-based 名称数组，按 UTF-8 字节序升序，每个资源名一次。
---仅含正在推进的实例；暂停/停止/自然结束排除，静音仍包含；修改数组无副作用。
---@return string[] names
function Audio.GetPlaying() end

---有正在推进的实例为 true；暂停/停止/从未播放为 false。先验证资源，不是存在查询。
---Play 成功后立即 true，Pause 后 false，Resume 后 true；自然结束可使相邻查询变化。
---@param name string
---@return boolean playing
function Audio.IsPlaying(name) end

---只能由 Audio.CreatePlayer 创建。未选中的不可达对象由 GC 兜底销毁。
---每个播放器同时容纳多个资源；同资源代次最多一个播放/暂停实例。
---所有方法只操作 self；不因调用、错误或对象内资源变化而选择 self。
---@class AudioPlayer
local AudioPlayer = {}

---从头播放已就绪 Sound/Music；同帧与模块入口共享去重，其他播放器独立。
---成功准备后才记录去重；失败抛错，不要求恢复先前被截断的实例。
---自然结束保留当帧标记；每帧无条件 Play Music 会持续从头重播。
---@param name string
function AudioPlayer:Play(name) end

---停止指定或全部实例并清除对应去重记录；省略/nil 为全部，不卸载资源。
---@param name? string
function AudioPlayer:Stop(name) end

---暂停指定或全部正在播放实例，保留位置/循环状态和去重记录。
---@param name? string
function AudioPlayer:Pause(name) end

---恢复指定或全部暂停实例；不重启正在播放者、不新建实例。
---@param name? string
function AudioPlayer:Resume(name) end

---设置本播放器音量，有限 [0,1]，默认 1；非法值不改变状态。
---对本播放器全部当前/后续实例生效，暂停者恢复使用新值。
---@param volume number
function AudioPlayer:SetVolume(volume) end

---读取本播放器自身音量。
---@return number volume
function AudioPlayer:GetVolume() end

---设置本播放器左右平衡，有限 [-1,1]，默认 0；语义与 Audio.SetPan 相同。
---@param pan number
function AudioPlayer:SetPan(pan) end

---读取本播放器自身声像。
---@return number pan
function AudioPlayer:GetPan() end

---返回本播放器正在推进的名称数组；独立副本、连续 1-based、UTF-8 字节序升序。
---@return string[] names
function AudioPlayer:GetPlaying() end

---先验证已发布 Sound/Music，再查询本播放器；暂停/停止/未播放为 false。
---@param name string
---@return boolean playing
function AudioPlayer:IsPlaying(name) end

---立即失效、停止全部实例并清除去重；若为当前播放器则取消选择，不影响别的播放器。
---不卸载资源；重复调用幂等。之后其他方法及 SetPlayer 使用此句柄均抛错。
---GC 与显式 Destroy 使用同一安全退休路径；及时停止应显式 Stop/Destroy。
function AudioPlayer:Destroy() end
