---@meta

-- Selen2D v2.0.0 一号机基线声明；仅供编辑器，不应执行。
-- 来源：lua_bind/modules/Audio/Audio.cpp、core/modules/Audio；设计依据：一号机/modules/Audio.md。
-- 全部接口严格检查参数数量和类型，成功无返回值（Getter 除外）；失效服务/资源/设备操作错误抛 Lua 错误。
-- name 为已加载且就绪的资源名（非路径），非空且无 NUL；资源不存在、类型不符、未准备完成均抛错。
-- 控制调用立即更新播放状态，实际可听变化受音频设备缓冲延迟影响；混音与流式补给不依赖游戏帧。
-- 资源卸载会停止关联播放；暂停游戏逻辑不会自动暂停声音。没有公开播放实例、设备或时间轴 API。
---@class AudioModule
Audio = {}

---从头单次播放 Sound；每次创建独立内部播放，可与同名先前播放重叠；不返回实例。
---@param name string 已就绪的 Sound 资源名。
function Audio.PlaySound(name) end

---停止此 Sound 的所有现有播放。有效资源没有实例时为空操作；不会停止其他资源或 Music。
---@param name string 已就绪的 Sound 资源名。
function Audio.StopSound(name) end

---暂停此 Sound 的所有现有实例，保留位置；没有实例时为空操作，之后 PlaySound 仍能新建播放。
---@param name string 已就绪的 Sound 资源名。
function Audio.PauseSound(name) end

---从各自暂停位置恢复此 Sound 的现有实例；不新建播放，没有实例时为空操作。
---@param name string 已就绪的 Sound 资源名。
function Audio.ResumeSound(name) end

---验证资源后停止当前 Music，并从头播放指定 Music；同名重播也重置位置，同一时间仅一首。
---播放到 Resource.LoadMusic 定义的循环终点后回到循环起点；默认循环整首。
---资源验证失败保留原音乐；验证后的新播放若失败，原音乐已停止，不承诺事务式恢复。
---@param name string 已就绪的 Music 资源名。
function Audio.PlayMusic(name) end

---停止并清除当前 Music；无当前音乐时为空操作。
function Audio.StopMusic() end

---暂停当前 Music，保留位置与循环状态；无当前音乐时为空操作。
function Audio.PauseMusic() end

---恢复当前暂停 Music；无当前音乐时为空操作，不自动开始新音乐。
function Audio.ResumeMusic() end

---设置 Sound 分类全局增益；默认 1。有限且在 [0,1]，越界抛错，不截断。
---对当前及后续 Sound 生效；有效增益为此值乘 Resource 单资源音量。
---@param volume number
function Audio.SetSoundVolume(volume) end

---读取 Sound 全局增益 [0,1]（默认 1），不乘单资源音量。
---@return number volume
function Audio.GetSoundVolume() end

---设置 Music 分类全局增益；默认 1。有限且在 [0,1]，越界抛错，不截断。
---对当前及后续 Music 生效，与 Sound 分类独立；有效增益乘 Resource 单资源音量。
---@param volume number
function Audio.SetMusicVolume(volume) end

---读取 Music 全局增益 [0,1]（默认 1），不乘单资源音量。
---@return number volume
function Audio.GetMusicVolume() end
