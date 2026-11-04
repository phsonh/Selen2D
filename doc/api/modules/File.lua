---@meta

-- Selen2D 二号机已实现 File 契约；编辑器定义，不参与运行。
-- 路径使用非空相对 VFS 路径、UTF-8/NFC 和正斜杠。拒绝绝对路径、反斜杠、
-- 独立 . / ..、空组件、末尾斜杠、NUL；不以进程 CWD 或脚本目录为根。
-- 只能访问编译配置接入的包/真实目录。未授权查询也抛错，不伪装不存在。
-- 文件句柄绑定打开时获准的后端。包只读；真实父目录不会由 Open 自动创建。
-- 参数/状态/路径/授权及异常 VFS 结构抛 Lua 错误；正常 I/O 失败返回独立错误表。
-- 除 GetScriptPath 一值、三个读取方法三值外，其余公开函数固定返回两值。
-- File 均读写原始字节；文本与二进制模式不隐式变换编码或换行。
-- require 保留原生缓存、preload、自定义搜索器。只在文件候选开头允许去掉一个 ./。
-- loadfile/dofile、标准 io 文件名、os.remove/rename、package.searchpath 共用 VFS 授权。
-- 标准 io 保留 LuaJIT 的返回形式，不能按本模块的 err 表解析；stdio/popen 保留。
-- loadlib/磁盘 C 搜索器、自动系统临时路径拒绝。FFI、popen、os.execute、应用主动
-- load/loadstring 和无文件名 stdin 编译不属于文件授权保证，绝非完整不可信代码沙箱。

---@alias FileOpenMode 'r'|'rb'|'r+'|'rb+'|'r+b'|'w'|'wb'|'w+'|'wb+'|'w+b'|'a'|'ab'|'a+'|'ab+'|'a+b'|'x'|'xb'|'x+'|'xb+'|'x+b'
---@alias FileSeekOrigin 'set'|'cur'|'end'
---@alias FileErrorCode 'not_found'|'already_exists'|'permission_denied'|'read_only'|'not_directory'|'is_directory'|'directory_not_empty'|'cross_device'|'no_space'|'resource_busy'|'out_of_range'|'name_conflict'|'corrupt_data'|'unsupported'|'io_error'
---@class FileError
---@field code FileErrorCode 稳定分类；范围外访问是抛错，不属于 permission_denied。
---@field message string 非空安全诊断；不保证固定措辞，不包含密钥或未授权物理路径。
-- 每次失败为独立、无元表且恰好含 code/message 的普通表；不存在 GetLastError。

---@class FileModule
File = {}

---同步打开文件，独立游标。r 读已有；w 创建/截断；a 每次写到末尾；x 排他创建。
---b 保持字节语义，+ 允许读写，二者可各一次。未创建父目录返回 nil,err。
---@param path string
---@param mode FileOpenMode
---@return FileHandle|nil file
---@return FileError|nil err
function File.Open(path, mode) end

---合法缺失为 false,nil；文件或目录存在为 true,nil；查询失败为 false,err。
---@param path string
---@return boolean exists
---@return FileError|nil err
function File.Exists(path) end

---目录/正常缺失为 false,nil；查询失败为 false,err。
---@param path string
---@return boolean isFile
---@return FileError|nil err
function File.IsFile(path) end

---包括虚拟挂载目录。文件/正常缺失为 false,nil。
---@param path string
---@return boolean isDirectory
---@return FileError|nil err
function File.IsDirectory(path) end

---直接子项名称，含文件/目录，不含父路径；非递归、去重、UTF-8 字节序排序。
---空目录 {},nil；缺失/非目录/普通失败 nil,err；空串及 . 不能代表根。
---@param path string
---@return string[]|nil entries
---@return FileError|nil err
function File.ListDirectory(path) end

---在获准真实范围内递归创建目录；已有目录 true,nil；失败可能留下部分祖先。
---@param path string
---@return boolean success
---@return FileError|nil err
function File.CreateDirectory(path) end

---删除可写普通文件或空目录，不递归；缺失、非空、只读等为 false,err。
---@param path string
---@return boolean success
---@return FileError|nil err
function File.Remove(path) end

---在指定目录重命名普通文件，不覆盖；oldName/newName 必须为合法单个名称。
---不能改名目录或跨目录移动；所有路径先授权，不创建父目录。
---@param directory string
---@param oldName string
---@param newName string
---@return boolean success
---@return FileError|nil err
function File.Rename(directory, oldName, newName) end

---复制普通文件，保留源；源可来自包；目标必须可写且不存在，不递归/不创建父目录。
---@param sourcePath string
---@param destinationPath string
---@return boolean success
---@return FileError|nil err
function File.Copy(sourcePath, destinationPath) end

---移动可写普通文件或目录，不覆盖。跨设备 false,cross_device；没有复制删除回退。
---@param sourcePath string
---@param destinationPath string
---@return boolean success
---@return FileError|nil err
function File.Move(sourcePath, destinationPath) end

---将可写源文件发布到目标文件；已存在则替换，否则创建；源/目标须在同一文件系统。
---不先删除旧目标；失败保留旧目标。成功不等于断电持久化，应先检查 Write 与 Close。
---@param sourcePath string
---@param destinationPath string
---@return boolean success
---@return FileError|nil err
function File.Replace(sourcePath, destinationPath) end

---重新读取并执行，忽略脚本返回值，无缓存；成功 true,nil，普通读失败 false,err。
---编译前检查代码来源；加密配置只能执行获准认证包内代码。语法/运行错误抛出，保留栈。
---@param path string
---@return boolean success
---@return FileError|nil err
function File.RunLuaFile(path) end

---返回直接 Lua 调用者的规范虚拟文件路径（含文件名），不探测当前文件是否仍存在。
---文件定义的闭包/协程保留来源；动态 loadstring 的 @名称不构成文件来源。
---不逐层回溯。尾调用/优化/元方法等无法可靠辨识的调用返回单个 nil，不归到上层脚本。
---@return string|nil path
function File.GetScriptPath() end

---只能由 File.Open 创建的原生 userdata；冒号调用。关闭后所有方法（再次 Close 也含）抛错。
---GC 仅兜底关闭；不会代替显式 Close 的失败检查。
---@class FileHandle
local FileHandle = {}

---最多读 size 字节；省略/nil 读剩余全部，0 成功空串且不移动游标。
---EOF 为 "",true,nil；短读成功；失败 nil,false,err，不交付部分结果，游标可能已推进。
---size 为 [0,2^53-1] 有限整数，错误参数抛错；实际读取还受内存限制。
---@param size? integer
---@return string|nil data
---@return boolean success
---@return FileError|nil err
function FileHandle:Read(size) end

---下一行，去掉 LF 或 CRLF；单独 CR 保留。空行 "",true,nil；EOF nil,true,nil。
---与其他读写/Seek/Tell 共用准确字节游标；普通失败 nil,false,err。
---@return string|nil line
---@return boolean success
---@return FileError|nil err
function FileHandle:ReadLine() end

---当前游标到 EOF 的行数组，末尾换行不制造空行；EOF {},true,nil。
---失败 nil,false,err，不交付已读前缀；内存或 Lua 数组索引限制抛错。
---@return string[]|nil lines
---@return boolean success
---@return FileError|nil err
function FileHandle:ReadLines() end

---写原始字节，支持 NUL；成功 true,nil，短写/只读/失败 false,err，可能已部分写入。
---空串仍检查可写性，不移动/扩展文件。不提供 Flush，必须检查 Close。
---@param data string
---@return boolean success
---@return FileError|nil err
function FileHandle:Write(data) end

---offset 为 [-(2^53-1),2^53-1] 有限整数。目标须在 [0,2^53-1]，越界 false,out_of_range。
---成功 true,nil；失败 false,err 且位置不变。可越过 EOF 定位，不立即扩展文件。
---@param offset integer
---@param whence? FileSeekOrigin 省略/nil 为 set；cur 当前，end 文件末尾。
---@return boolean success
---@return FileError|nil err
function FileHandle:Seek(offset, whence) end

---零基字节位置，范围 [0,2^53-1]，不改变游标；失败 nil,err。
---@return integer|nil position
---@return FileError|nil err
function FileHandle:Tell() end

---关闭并释放原生流；成功 true,nil，提交/关闭失败 false,err；无论结果立即失效。
---@return boolean success
---@return FileError|nil err
function FileHandle:Close() end
