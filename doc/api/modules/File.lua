---@meta

-- Selen2D v2.0.0 一号机基线声明；仅供编辑器，不应执行。
-- 来源：lua_bind/modules/File/File.cpp、core/modules/File/Vfs.cpp、core/runtime/Lua/VfsScripts.cpp。
-- 设计依据：devs/design/Solongo API 一号机/modules/File.md，及已批准的安装根普通目录写入行为。
-- 路径相对安装/虚拟根，支持 UTF-8，/ 与反斜杠均规范化；最多 4096 字节，拒绝 NUL、非法 UTF-8、绝对路径。
-- . 与重复分隔符归一化；.. 不得越过虚拟根或挂载边界；拒绝 Windows 设备名、非法组件和符号链接/重解析点。
-- 普通未挂载目录可读写，由 Lua 决定存储目录。挂载目标、其祖先、源物理别名受保护，挂载内容只读。
-- 后挂载同路径覆盖前挂载，查询读取统一 VFS；挂载下不存在的路径不会回退到无关物理文件。
-- 本模块同步完成操作。错误实参数量/类型、非法模式、无效句柄会抛 Lua 错误；正常 I/O 失败见各返回值。
-- 无默认文件路径；文件父目录需先创建。所有 I/O 均为原始字节，不自动换行或转换编码。

---@alias FileOpenMode 'r'|'rb'|'r+'|'rb+'|'r+b'|'w'|'wb'|'w+'|'wb+'|'w+b'|'a'|'ab'|'a+'|'ab+'|'a+b'|'x'|'xb'|'x+'|'xb+'|'x+b'
---@alias FileSeekOrigin 'set'|'cur'|'end'
---@class FileModule
File = {}

---同步打开普通文件，成功返回有独立游标的句柄；路径非法、只读限制或操作失败返回 nil。
---mode 必填：r 读取已有文件；w 创建/截断；a 创建/末尾追加；x 仅在不存在时排他创建。
---主模式后可各加一次 b 和 +，顺序任意；+ 读写，b 不改变原始字节语义；任何可写模式均不能访问挂载内容。
---不创建父目录。mode 非法或参数错误抛错；不能通过扩展名区分真实目录与包挂载目录。
---@param path string
---@param mode FileOpenMode
---@return FileHandle|nil file
function File.Open(path, mode) end

---读取 VFS 视图：文件或目录（含挂载目录）存在为 true；无效路径/不可查询为 false。
---@param path string
---@return boolean exists
function File.Exists(path) end

---路径在 VFS 中为普通文件时 true；目录、挂载点、缺失或无效路径为 false。
---@param path string
---@return boolean isFile
function File.IsFile(path) end

---路径在 VFS 中为目录时 true，包含虚拟挂载点；缺失或无效路径为 false。
---@param path string
---@return boolean isDirectory
function File.IsDirectory(path) end

---返回直接子项名称数组（从 1 索引，不带父路径），文件与目录均包含；不递归，同名去重。
---空目录为 {}；无效路径、不是目录或列举失败为 nil。基线按名称排序，不依赖该顺序组织游戏逻辑。
---@param path string 空串或 "." 可表示虚拟根。
---@return string[]|nil entries
function File.ListDirectory(path) end

---递归创建可写普通目录；已存在目录为 true，目标是文件、受保护或操作失败为 false。
---@param path string
---@return boolean success
function File.CreateDirectory(path) end

---删除可写普通文件或空目录；不递归。缺失、非空目录、受保护路径或 I/O 失败返回 false。
---@param path string
---@return boolean success
function File.Remove(path) end

---只在指定目录内重命名普通文件；不覆盖，不能用于目录改名或跨目录移动。
---oldName/newName 必须为单个合法文件名（允许通常扩展名，不允许分隔符、独立 . 或 ..）。
---源不存在、目标已存在、不可写、名称非法或操作失败为 false；不创建父目录。
---@param directory string
---@param oldName string
---@param newName string
---@return boolean success
function File.Rename(directory, oldName, newName) end

---复制普通文件内容，保留源；源可来自只读挂载，目标必须可写且不存在，不创建父目录。
---不复制目录树；失败返回 false。失败时尝试移除本次新建的未完成目标，不覆盖既有目标。
---@param sourcePath string
---@param destinationPath string
---@return boolean success
function File.Copy(sourcePath, destinationPath) end

---移动可写普通文件或目录，目标必须不存在，不创建父目录。
---基线使用系统无覆盖重命名；跨卷/不支持/受保护或操作失败返回 false，保留源，不作复制删除回退。
---@param sourcePath string
---@param destinationPath string
---@return boolean success
function File.Move(sourcePath, destinationPath) end

---把可写普通源文件移动发布到目标文件；目标存在则替换，不存在则创建，成功后源路径消失。
---要求同一文件系统、目标父目录存在且目标不是目录；失败保留旧目标，不使用先删目标再移动。
---返回 true 表示替换调用成功，不等于断电持久化；发布前检查 Write 与 Close 结果。
---@param sourcePath string
---@param destinationPath string
---@return boolean success
function File.Replace(sourcePath, destinationPath) end

---每次重新从 VFS 读取并在当前 Lua VM/全局环境执行；同步、无缓存，不改变工作目录。
---读取失败为 false，编译和执行成功为 true；脚本所有返回值被丢弃，return false 仍算成功。
---语法/运行时错误抛出并保留调用栈，不能伪装为 false；不等价于 require 的缓存机制。
---@param path string
---@return boolean success
function File.RunLuaFile(path) end

---原生 userdata；只能由 File.Open 创建，不提供公开构造器。
---方法用冒号调用。Close 后所有方法（再次 Close 也包括）抛错；GC 兜底关闭不能报告写入失败。
---@class FileHandle
local FileHandle = {}

---从当前位置读最多 size 个字节并推进游标；省略/nil 读完剩余内容，0 返回空串成功。
---EOF/短读成功返回实际内容与 true，已在 EOF 返回 "",true；不可读或读取失败返回 nil,false。
---size 必须为非负有限整数且小于本机 size_t 的 2^位数；大读取仍受内存限制，分配失败抛错。
---@param size? integer 字节数，不是 UTF-8 字符数；无默认上限。
---@return string|nil data
---@return boolean success
function FileHandle:Read(size) end

---从游标读下一行，去掉 LF 及其前的 CR；不把单独 CR 作为行分隔符。
---空行返回 "",true；EOF 且无内容返回 nil,true；不可读/I/O 失败返回 nil,false。
---没有终止换行的末行也会返回；读写、Seek、Tell 与逐行读取共享同一准确字节游标。
---@return string|nil line
---@return boolean success
function FileHandle:ReadLine() end

---从当前位置读到 EOF，返回去除 LF/CRLF 的连续字符串数组；已在 EOF 返回 {},true。
---末尾换行不额外制造空行，中间空行保留；失败返回 nil,false，不交付部分结果，游标可能已推进。
---结果超过 Lua 数组索引范围或内存不足抛错。
---@return string[]|nil lines
---@return boolean success
function FileHandle:ReadLines() end

---写入字符串原始字节，允许 NUL，不添加换行；a 模式始终追加，其余按游标写入。
---不可写、短写、I/O 失败返回 false，可能已写入部分数据；成功为 true，包括可写流的空串。
---成功不保证磁盘持久化；必须检查 Close，不提供独立 Flush。
---@param data string
---@return boolean success
function FileHandle:Write(data) end

---按指定基准改变字节游标；成功 true，定位失败 false，set 的负偏移直接 false。
---offset 必须是有限整数且位于 [-2^63,2^63)；非法 origin/参数抛错。支持 VFS 包逻辑定位。
---Lua number 在超过 2^53 的偏移处不再逐字节精确；越过 EOF 的支持由流实现决定。
---@param offset integer
---@param whence? FileSeekOrigin 省略或 nil 默认为 "set"；cur 相对当前位置，end 相对结尾。
---@return boolean success
function FileHandle:Seek(offset, whence) end

---返回当前零基字节位置；查询失败 nil。不改变游标，超过 2^53 的位置可能受 Lua number 精度限制。
---@return integer|nil position
function FileHandle:Tell() end

---完成缓冲关闭并释放句柄；成功 true，提交/关闭失败 false。
---不论返回值均立即使句柄失效；重复关闭抛错，不是幂等操作。GC 自动关闭不能替代显式检查。
---@return boolean success
function FileHandle:Close() end
