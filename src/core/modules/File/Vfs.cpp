#include "Vfs.h"
#include "Package.h"
#include "NativeIo.h"
#include "ReadSource.h"
#include "Profile.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <exception>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#undef CreateDirectory
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <stdio.h>
#endif
#endif

namespace core::modules::File {
namespace {
namespace fs=std::filesystem;
std::string Utf8(const fs::path& path) {
    const auto s=path.generic_u8string(); return {reinterpret_cast<const char*>(s.data()),s.size()};
}
fs::path NativePath(std::string_view text) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()),text.size()));
}
[[noreturn]] void Fail(std::string code,std::string_view operation,std::string_view path) {
    throw IoError(code,std::string(operation)+" '"+std::string(path)+"': "+code);
}
// Preserve the precise native reason internally. Only a directory sharing
// violation can postpone a scan; other resource_busy/permission errors cannot.
struct SharingConflict final : IoError {
    SharingConflict(std::string_view operation,std::string_view path)
        : IoError("resource_busy",std::string(operation)+" '"+std::string(path)+"': resource_busy (Win32 32)") {}
};
void Check(const std::error_code& error,std::string_view operation,std::string_view path) {
#ifdef _WIN32
    if(error.category()==std::system_category()&&error.value()==ERROR_SHARING_VIOLATION)
        throw SharingConflict(operation,path);
#endif
    if(error) Fail(IoErrorCode(error),operation,path);
}
std::error_code LastError() {
#ifdef _WIN32
    return {static_cast<int>(GetLastError()),std::system_category()};
#else
    return {errno,std::system_category()};
#endif
}
std::string Strict(std::string_view path) {
    auto normalized=Normalize(path);
    if(!normalized) throw InvalidPath("expected a nonempty canonical relative UTF-8 path");
    return std::move(*normalized);
}
std::string Parent(std::string_view path) {
    const auto slash=path.rfind('/'); return slash==std::string_view::npos?std::string{}:std::string(path.substr(0,slash));
}
std::string Leaf(std::string_view path) { return std::string(path.substr(path.rfind('/')==std::string_view::npos?0:path.rfind('/')+1)); }
std::string Join(std::string_view parent,std::string_view child) {
    if(parent.empty()) return std::string(child);
    if(child.empty()) return std::string(parent);
    return std::string(parent)+"/"+std::string(child);
}
std::string Tail(std::string_view parent,std::string_view path) {
    if(parent.empty()) return std::string(path);
    return parent==path?std::string{}:std::string(path.substr(parent.size()+1));
}
void NativeName(std::string_view relative,std::string_view virtualPath) {
#ifdef _WIN32
    for(std::size_t start=0;start<relative.size();) {
        auto end=relative.find('/',start); if(end==std::string_view::npos) end=relative.size();
        const auto part=relative.substr(start,end-start);
        if(part.empty()||part.back()=='.'||part.back()==' '||part.find_first_of("\"<>|")!=std::string_view::npos)
            Fail("unsupported","Windows filename",virtualPath);
        std::string stem(part.substr(0,part.find('.')));
        for(auto& c:stem) if(c>='a'&&c<='z') c-=32;
        if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL"||stem=="CONIN$"||stem=="CONOUT$")
            Fail("unsupported","Windows filename",virtualPath);
        if(stem.starts_with("COM")||stem.starts_with("LPT")) {
            const auto suffix=std::string_view(stem).substr(3);
            if((suffix.size()==1&&suffix[0]>='1'&&suffix[0]<='9')||suffix=="\xc2\xb9"||suffix=="\xc2\xb2"||suffix=="\xc2\xb3")
                Fail("unsupported","Windows filename",virtualPath);
        }
        start=end+1;
    }
#else
    (void)relative; (void)virtualPath;
#endif
}
std::string PhysicalKey(const fs::path& path) {
    auto result=Utf8(path.lexically_normal());
    const auto rootLength=Utf8(path.root_path()).size();
    while(result.size()>rootLength&&result.back()=='/') result.pop_back();
#ifdef _WIN32
    return FoldPath(result);
#else
    return result;
#endif
}
bool PhysicalContains(const fs::path& parent,const fs::path& child) {
    auto a=PhysicalKey(parent),b=PhysicalKey(child);
    while(a.size()>1 && a.back()=='/') a.pop_back();
    return a==b || (b.size()>a.size() && b.starts_with(a) && (a.back()=='/'||b[a.size()]=='/'));
}
bool Redirected(const fs::path& path,const fs::file_status& status) {
    if(fs::is_symlink(status)) return true;
#ifdef _WIN32
    VFS_PROFILE_ADD(AttributeQueries,1);
    const auto attrs=GetFileAttributesW(path.c_str());
    return attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_REPARSE_POINT)!=0;
#else
    (void)path; return false;
#endif
}
fs::file_status Status(const fs::path& path,std::string_view virtualPath) {
    std::error_code ec;
    VFS_PROFILE_ADD(AttributeQueries,1);
    const auto status=fs::symlink_status(path,ec);
    if(ec==std::errc::no_such_file_or_directory || ec==std::errc::not_a_directory) return fs::file_status(fs::file_type::not_found);
    Check(ec,"inspect",virtualPath);
    if(Redirected(path,status)) throw AccessError("redirected filesystem path refused at '"+std::string(virtualPath)+"'");
    return status;
}
// Keep each visited directory open while checking/using a view. On Windows the
// missing DELETE share prevents renaming an already checked parent during I/O.
// Configured base/project roots also retain such anchors across operations.
struct DirectoryAnchor {
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
    explicit DirectoryAnchor(const fs::path& path,std::string_view virtualPath) {
        VFS_PROFILE_ADD(AnchorCreations,1);
        handle=CreateFileW(path.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
            OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(handle==INVALID_HANDLE_VALUE) Check(LastError(),"anchor directory",virtualPath);
        BY_HANDLE_FILE_INFORMATION info{};
        VFS_PROFILE_ADD(AttributeQueries,1);
        if(!GetFileInformationByHandle(handle,&info)) {
            const auto ec=LastError(); CloseHandle(handle); handle=INVALID_HANDLE_VALUE;
            Fail(IoErrorCode(ec),"anchor directory",virtualPath);
        }
        if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(handle); handle=INVALID_HANDLE_VALUE;
            throw AccessError("directory identity is redirected or changed at '"+std::string(virtualPath)+"'");
        }
    }
    ~DirectoryAnchor() { if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
    int handle{-1};
    explicit DirectoryAnchor(const fs::path& path,std::string_view virtualPath) {
        VFS_PROFILE_ADD(AnchorCreations,1);
        handle=::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(handle<0) Fail(IoErrorCode(LastError()),"anchor directory",virtualPath);
    }
    ~DirectoryAnchor() { if(handle>=0) ::close(handle); }
#endif
    DirectoryAnchor(const DirectoryAnchor&)=delete;
    DirectoryAnchor& operator=(const DirectoryAnchor&)=delete;
    bool Same(const DirectoryAnchor& other) const {
#ifdef _WIN32
        BY_HANDLE_FILE_INFORMATION a{},b{};
        VFS_PROFILE_ADD(IdentityQueries,1);
        if(!GetFileInformationByHandle(handle,&a))
            throw ConflictError("cannot verify configured directory identity");
        VFS_PROFILE_ADD(IdentityQueries,1);
        if(!GetFileInformationByHandle(other.handle,&b))
            throw ConflictError("cannot verify configured directory identity");
        return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow;
#else
        struct stat a{},b{};
        VFS_PROFILE_ADD(IdentityQueries,1);
        if(fstat(handle,&a)) throw ConflictError("cannot verify configured directory identity");
        VFS_PROFILE_ADD(IdentityQueries,1);
        if(fstat(other.handle,&b)) throw ConflictError("cannot verify configured directory identity");
        return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;
#endif
    }
};
using Anchor=std::shared_ptr<DirectoryAnchor>;
struct GuardedDirectory { fs::path path; Anchor anchor; };
using Guards=std::vector<GuardedDirectory>;
Anchor Guard(const fs::path& path,std::string_view virtualPath,Guards& guards) {
    auto anchor=std::make_shared<DirectoryAnchor>(path,virtualPath);
    guards.push_back({path,anchor}); return anchor;
}
fs::path BaseRoot(const fs::path& path) {
    if(!path.is_absolute()) throw InvalidPath("a configured filesystem base must be absolute");
    std::error_code ec;
    const auto root=fs::canonical(path,ec);
    Check(ec,"resolve configured base","<configured root>");
    VFS_PROFILE_ADD(AttributeQueries,1);
    if(!fs::is_directory(root,ec)) { Check(ec,"resolve configured base","<configured root>"); throw InvalidPath("configured base is not a directory"); }
#ifdef _WIN32
    DirectoryAnchor anchor(root,"<configured root>");
    const auto length=GetFinalPathNameByHandleW(anchor.handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!length) Fail(IoErrorCode(LastError()),"resolve configured base","<configured root>");
    std::wstring name(length,L'\0');
    const auto written=GetFinalPathNameByHandleW(anchor.handle,name.data(),length,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!written||written>=length) Fail("io_error","resolve configured base","<configured root>");
    name.resize(written); return fs::path(name);
#else
    return root;
#endif
}
// Match a normalized component against actual spelling, without Windows' case
// insensitive lookup or 8.3 aliases. Detect normalization/casefold collisions.
std::optional<fs::path> Child(const fs::path& directory,std::string_view component,std::string_view virtualPath) {
    VFS_PROFILE_SCOPE(Child);
    VFS_PROFILE_ADD(DirectoryEnumerations,1);
    std::error_code ec; fs::directory_iterator it(directory,ec),end;
    Check(ec,"enumerate",virtualPath);
    const auto wantedFold=FoldPath(component);
    std::optional<std::string> spelling;
    std::optional<fs::path> result;
    while(it!=end) {
        VFS_PROFILE_ADD(EntriesEnumerated,1);
        const auto actual=Utf8(it->path().filename()); const auto name=Normalize(actual);
        // Ancestors outside the project are not part of the virtual tree.
        // Ignore unrelated project names and inspect only this component.
        if(name&&FoldPath(*name)==wantedFold) {
            if(spelling) throw ConflictError("Unicode or case collision in '"+std::string(virtualPath)+"'");
            spelling=actual;
            if(*name!=component) throw ConflictError("configured path has incorrect case at '"+std::string(virtualPath)+"'");
            result=it->path();
        }
        it.increment(ec); Check(ec,"enumerate",virtualPath);
    }
    return result;
}
// Resolve a configured project/source path, checking only the declared chain.
// Missing trailing components keep an exact future creation route.
fs::path ProjectPath(const fs::path& base,std::string_view relative,std::string_view virtualPath,Guards& guards) {
    NativeName(relative,virtualPath);
    auto result=base; bool missing=false;
    for(std::size_t start=0;start<relative.size();) {
        auto end=relative.find('/',start); if(end==std::string_view::npos) end=relative.size();
        const auto part=relative.substr(start,end-start);
        if(!missing) {
            const auto status=Status(result,virtualPath);
            if(!fs::is_directory(status)) Fail("not_directory","resolve mapping",virtualPath);
            Guard(result,virtualPath,guards);
            const auto child=Child(result,part,virtualPath);
            if(child) result=*child;
            else { result/=NativePath(part); missing=true; }
        } else result/=NativePath(part);
        if(!missing) (void)Status(result,virtualPath);
        start=end+1;
    }
    return result;
}
using FileIdentity=std::array<std::uint64_t,3>;
struct DiskEntry { NodeInfo info; std::optional<FileIdentity> identity; std::uint64_t links{}; };
#if !defined(_WIN32) || !defined(_MSC_VER)
NodeInfo DiskInfo(const fs::path& disk,const fs::file_status& status,std::string_view path) {
    if(fs::is_directory(status)) return {NodeType::Directory};
    if(!fs::exists(status)) return {};
    if(!fs::is_regular_file(status)) throw ConflictError("non-regular filesystem entry at '"+std::string(path)+"'");
    VFS_PROFILE_ADD(AttributeQueries,1);
    std::error_code ec; const auto size=fs::file_size(disk,ec); Check(ec,"size",path);
    VFS_PROFILE_ADD(AttributeQueries,1);
    const auto modified=fs::last_write_time(disk,ec); Check(ec,"timestamp",path);
    return {NodeType::File,size,static_cast<std::uint64_t>(modified.time_since_epoch().count())};
}
FileIdentity Identity(const fs::path& disk,std::string_view path) {
#ifdef _WIN32
    const auto handle=CreateFileW(disk.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle==INVALID_HANDLE_VALUE) Fail(IoErrorCode(LastError()),"inspect file identity",path);
    FILE_ID_INFO info{};
    VFS_PROFILE_ADD(IdentityQueries,1);
    if(!GetFileInformationByHandleEx(handle,FileIdInfo,&info,sizeof(info))) {
        const auto ec=LastError(); CloseHandle(handle); Fail(IoErrorCode(ec),"inspect file identity",path);
    }
    CloseHandle(handle);
    FileIdentity result{info.VolumeSerialNumber,0,0};
    std::memcpy(&result[1],info.FileId.Identifier,16); return result;
#else
    VFS_PROFILE_ADD(IdentityQueries,1);
    struct stat info{}; if(lstat(disk.c_str(),&info)) Fail(IoErrorCode(LastError()),"inspect file identity",path);
    return {static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino),0};
#endif
}
#endif
DiskEntry InspectDisk(const fs::path& disk,std::string_view path) {
#if defined(_WIN32) && defined(_MSC_VER)
    // Query live metadata and identity through one handle. Directory enumeration
    // metadata is allowed to be stale and is never an authorization substitute.
    auto handle=CreateFileW(disk.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    auto error=handle==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;
    bool directoryOnly=false;
    if(handle==INVALID_HANDLE_VALUE&&error==ERROR_ACCESS_DENIED) {
        // Win32 requires BACKUP_SEMANTICS to open directories. Do not apply it
        // to regular files: an enabled SeBackupPrivilege could bypass the ACL
        // that the original ordinary file identity open was required to obey.
        VFS_PROFILE_ADD(AttributeQueries,1);
        const auto attributes=GetFileAttributesW(disk.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)) {
            directoryOnly=true;
            handle=CreateFileW(disk.c_str(),FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(handle==INVALID_HANDLE_VALUE) error=GetLastError();
        }
    }
    if(handle==INVALID_HANDLE_VALUE) {
        if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND||error==ERROR_DIRECTORY) return {};
        Fail(IoErrorCode({static_cast<int>(error),std::system_category()}),"inspect",path);
    }
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{handle};
    BY_HANDLE_FILE_INFORMATION info{};
    VFS_PROFILE_ADD(AttributeQueries,1);
    if(!GetFileInformationByHandle(handle,&info)) Fail(IoErrorCode(LastError()),"inspect",path);
    if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)
        throw AccessError("redirected filesystem path refused at '"+std::string(path)+"'");
    if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) return {{NodeType::Directory},{}};
    if(directoryOnly) throw ConflictError("directory type changed at '"+std::string(path)+"'");
    if(GetFileType(handle)!=FILE_TYPE_DISK)
        throw ConflictError("non-regular filesystem entry at '"+std::string(path)+"'");
    FILE_ID_INFO id{};
    VFS_PROFILE_ADD(IdentityQueries,1);
    if(!GetFileInformationByHandleEx(handle,FileIdInfo,&id,sizeof(id)))
        Fail(IoErrorCode(LastError()),"inspect file identity",path);
    FileIdentity identity{id.VolumeSerialNumber,0,0};
    std::memcpy(&identity[1],id.FileId.Identifier,16);
    // MSVC filesystem::file_time_type uses Windows FILETIME's epoch and ticks.
    // Keep the existing Stat.modified value rather than converting to Unix time.
    static_assert(std::ratio_equal_v<fs::file_time_type::period,std::ratio<1,10000000>>);
    const auto modified=(std::uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
    const auto size=(std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    return {{NodeType::File,size,modified},identity,info.nNumberOfLinks};
#else
    const auto status=Status(disk,path);
    const auto info=DiskInfo(disk,status,path);
    return {info,info.type==NodeType::File?std::optional<FileIdentity>(Identity(disk,path)):std::nullopt};
#endif
}
struct StreamCloser { void operator()(SDL_IOStream* value) const { if(value) SDL_CloseIO(value); } };
using Stream=std::unique_ptr<SDL_IOStream,StreamCloser>;
void Cancel(const ReadOptions& options) { if(options.cancelled&&options.cancelled()) throw ReadCancelled(); }
void RenamePhysical(const fs::path& source,const fs::path& target,bool replace,std::string_view name) {
#ifdef _WIN32
    if(!MoveFileExW(source.c_str(),target.c_str(),replace?MOVEFILE_REPLACE_EXISTING:0))
        Fail(IoErrorCode(LastError()),replace?"replace":"move",name);
#elif defined(__linux__) && defined(SYS_renameat2)
    if(syscall(SYS_renameat2,AT_FDCWD,source.c_str(),AT_FDCWD,target.c_str(),replace?0:1))
        Fail(IoErrorCode(LastError()),replace?"replace":"move",name);
#elif defined(__APPLE__)
    if(renamex_np(source.c_str(),target.c_str(),replace?0:RENAME_EXCL))
        Fail(IoErrorCode(LastError()),replace?"replace":"move",name);
#else
    if(!replace) Fail("unsupported","move",name);
    if(::rename(source.c_str(),target.c_str())) Fail(IoErrorCode(LastError()),"replace",name);
#endif
}
}

OpenMode ParseMode(std::string_view mode) {
    if(mode.empty()||std::string_view("rwax").find(mode.front())==std::string_view::npos)
        throw std::invalid_argument("File.Open mode must start with r, w, a or x");
    bool binary{},plus{};
    for(std::size_t i=1;i<mode.size();++i) {
        if(mode[i]=='b'&&!binary) binary=true;
        else if(mode[i]=='+'&&!plus) plus=true;
        else throw std::invalid_argument("invalid File.Open mode");
    }
    OpenMode result; result.base=mode.front(); result.readable=result.base=='r'||plus;
    result.writable=result.base!='r'||plus; result.append=result.base=='a'; result.exclusive=result.base=='x';
    result.sdlMode=std::string(1,result.exclusive?'w':result.base)+"b"+(plus?"+":"")+(result.exclusive?"x":"");
    return result;
}

struct FileSystem::Impl {
    struct Mount { fs::path source; std::string target; std::uint32_t parts{}; bool encrypted{}; std::shared_ptr<Package> package; };
    struct Directory { fs::path base,root; std::string path,target; Anchor baseAnchor; mutable Anchor rootAnchor; };
    struct Node {
        NodeInfo info;
        const Mount* mount{};
        const Directory* directory{};
        fs::path disk;
        std::string relative;
        std::optional<FileIdentity> identity;
        std::uint64_t links{};
    };
    struct View {
        std::map<std::string,Node,std::less<>> nodes;
        std::map<std::string,std::string,std::less<>> folded;
        std::map<FileIdentity,const Directory*> identities;
        Guards guards;
        struct Pending { std::string path; const Directory* directory; std::string failure; };
        std::vector<Pending> pending;
        void RequireComplete(std::string_view path,bool subtree=false) const {
            for(const auto& entry:pending)
                if(PathContains(entry.path,path)||(subtree&&PathContains(path,entry.path)))
                    throw IoError("resource_busy",entry.failure);
        }
        bool NeedsSingleLink(const Directory* directory) const {
            return std::any_of(pending.begin(),pending.end(),[&](const auto& entry){return entry.directory!=directory;});
        }
        void RequireAliases(const Node* node,const Directory* destination=nullptr) const {
            if(node&&node->identity&&node->links!=1&&NeedsSingleLink(destination?destination:node->directory))
                Fail("resource_busy","cannot verify cross-mapping aliases while a directory is pending",node->disk.empty()?"<file>":Utf8(node->disk.filename()));
        }
        const Node* Find(std::string_view path) const {
            const auto it=nodes.find(path); return it==nodes.end()?nullptr:&it->second;
        }
        const Node* Resolve(std::string_view path) const {
            for(auto parent=Parent(path);!parent.empty();parent=Parent(parent)) {
                const auto* node=Find(parent);
                if(node&&node->info.type!=NodeType::Directory) Fail("not_directory","resolve",path);
            }
            return Find(path);
        }
        void Insert(std::string path,Node node,bool packageOverlay=false) {
            const auto canonical=Normalize(path);
            // Only the internal deployment-root node may have an empty name.
            // It is not addressable by an empty/dot Lua path.
            if(path.empty()) {
                if(node.info.type!=NodeType::Directory || node.mount)
                    throw ConflictError("invalid internal root node");
            } else if(!canonical||*canonical!=path) throw ConflictError("mapped entry exceeds the canonical path contract");
            if(node.identity&&node.directory) {
                const auto [source,added]=identities.emplace(*node.identity,node.directory);
                if(!added&&source->second!=node.directory)
                    throw ConflictError("same physical file is authorized through different targets at '"+path+"'");
            }
            const auto fold=FoldPath(path);
            const auto [key,added]=folded.emplace(fold,path);
            if(!added&&key->second!=path) throw ConflictError("Unicode case collision at '"+path+"'");
            const auto [it,inserted]=nodes.emplace(path,node);
            if(!inserted) {
                auto& old=it->second;
                if(old.info.type!=node.info.type) throw ConflictError("file/directory conflict at '"+path+"'");
                if(node.info.type==NodeType::File) {
                    if(!packageOverlay||!old.mount||!node.mount) throw ConflictError("multiple file sources at '"+path+"'");
                    old=std::move(node);
                } else if(node.directory) {
                    if(old.directory&&PhysicalKey(old.disk)!=PhysicalKey(node.disk)) throw ConflictError("multiple directory destinations at '"+path+"'");
                    old.directory=node.directory; old.disk=std::move(node.disk);
                }
            }
            for(auto ancestor=Parent(path);!ancestor.empty();ancestor=Parent(ancestor)) {
                const auto existing=nodes.find(ancestor);
                if(existing!=nodes.end()&&existing->second.info.type!=NodeType::Directory)
                    throw ConflictError("file blocks directory at '"+ancestor+"'");
                const auto [foldIt,foldAdded]=folded.emplace(FoldPath(ancestor),ancestor);
                if(!foldAdded&&foldIt->second!=ancestor) throw ConflictError("Unicode case collision at '"+ancestor+"'");
                nodes.try_emplace(ancestor,Node{NodeInfo{NodeType::Directory}});
            }
        }
        void UnlockSubtree(const fs::path& root) {
            std::erase_if(guards,[&](const auto& guard){ return PhysicalContains(root,guard.path); });
        }
    };
    fs::path install;
    std::vector<Mount> mounts;
    std::vector<Directory> directories;
    bool protectedCode{};
    mutable std::recursive_mutex mutex;

    std::string Authorize(std::string_view path) const {
        auto p=Strict(path);
        for(const auto& d:directories) if(PathContains(d.target,p)||PathContains(p,d.target)) return p;
        for(const auto& m:mounts) if(PathContains(m.target,p)||PathContains(p,m.target)) return p;
        throw AccessError("path is outside the compiled authorization: '"+p+"'");
    }
    const Directory& Route(std::string_view path) const {
        for(const auto& d:directories) if(PathContains(d.target,path)) return d;
        throw AccessError("no authorized real directory for '"+std::string(path)+"'");
    }
    bool Protected(std::string_view path) const {
        for(const auto& d:directories) if(PathContains(path,d.target)) return true;
        for(const auto& m:mounts) if(PathContains(path,m.target)) return true;
        return false;
    }
    void CheckBacking(const fs::path& disk,std::string_view path,bool subtree) const {
        for(const auto& m:mounts) {
            if((subtree&&PhysicalContains(disk,m.source)) || PhysicalKey(disk)==PhysicalKey(m.source) || m.package->IsBackingFile(disk))
                Fail("read_only","modify active package backend",path);
        }
    }
    void CheckCode(const View& view,std::string_view path) const {
        const auto* node=view.Resolve(path);
        if(protectedCode&&node&&node->info.type==NodeType::File&&(!node->mount||!node->mount->encrypted))
            throw AccessError("file code is not authenticated package content at '"+std::string(path)+"'");
    }
    void AddDisk(View& view,const Directory& d,const fs::path& disk,const std::string& path,bool deferSharing) const {
        VFS_PROFILE_SCOPE(AddDisk);
        std::vector<std::pair<fs::path,std::string>> pending{{disk,path}};
        while(!pending.empty()) {
            auto [current,name]=std::move(pending.back()); pending.pop_back();
            const auto entry=InspectDisk(current,name);
            const auto info=entry.info;
            if(info.type==NodeType::Missing) continue;
            Node node{info,nullptr,&d,current,{}};
            if(info.type==NodeType::File) {
                VFS_PROFILE_ADD(FilesVisited,1);
                node.identity=entry.identity;
                node.links=entry.links;
            }
            view.Insert(name,std::move(node));
            if(info.type!=NodeType::Directory) continue;
            VFS_PROFILE_ADD(DirectoriesVisited,1);
            try {
                auto anchor=Guard(current,name,view.guards);
                if(name==d.target) {
                    if(d.rootAnchor&&!d.rootAnchor->Same(*anchor)) throw ConflictError("configured root identity changed at '"+name+"'");
                    if(!d.rootAnchor) d.rootAnchor=anchor;
                }
                VFS_PROFILE_ADD(DirectoryEnumerations,1);
                std::error_code ec; fs::directory_iterator it(current,ec),end;
                Check(ec,"enumerate",name);
                std::set<std::string,std::less<>> originals;
                while(it!=end) {
                    VFS_PROFILE_ADD(EntriesEnumerated,1);
                    const auto child=Normalize(Utf8(it->path().filename()));
                    if(!child||child->find('/')!=std::string::npos) throw ConflictError("unaddressable directory entry at '"+name+"'");
                    if(!originals.insert(FoldPath(*child)).second) throw ConflictError("Unicode collision at '"+name+"'");
                    pending.emplace_back(it->path(),Join(name,*child));
                    it.increment(ec); Check(ec,"enumerate",name);
                }
            } catch(const SharingConflict& error) {
                if(!deferSharing) throw;
                // Recheck observable type/reparse state before deferring. This
                // is unknown subtree content, never a cached safe subtree.
                if(InspectDisk(current,name).info.type!=NodeType::Directory)
                    throw ConflictError("directory changed during validation at '"+name+"'");
                view.pending.push_back({name,&d,error.what()});
            }
        }
    }
    View Scan(bool deferSharing=true) const {
        VFS_PROFILE_SCOPE(Scan);
        View view;
        // All package records are checked before resolving overlays. A real
        // file collision cannot be hidden by a higher-parts package record.
        for(const auto& m:mounts) {
            VFS_PROFILE_ADD(PackageViewPasses,1);
            view.Insert(m.target,Node{NodeInfo{NodeType::Directory},&m});
            for(const auto& [relative,info]:m.package->Entries()) {
                if(relative.empty()) continue;
                VFS_PROFILE_ADD(PackageViewEntries,1);
                view.Insert(m.target+"/"+relative,Node{info,&m,nullptr,{},relative},true);
            }
        }
        for(const auto& d:directories) {
            const auto baseAnchor=Guard(d.base,d.target,view.guards);
            if(!d.baseAnchor->Same(*baseAnchor)) throw ConflictError("configured base identity changed at '"+d.target+"'");
            view.Insert(d.target,Node{NodeInfo{NodeType::Directory}});
            const auto root=ProjectPath(d.base,d.path,d.target,view.guards);
            if(PhysicalKey(root)!=PhysicalKey(d.root)) throw ConflictError("configured root identity changed at '"+d.target+"'");
            const auto status=Status(root,d.target);
            if(fs::exists(status)&&!fs::is_directory(status)) throw ConflictError("configured directory root is occupied by a file at '"+d.target+"'");
            if(fs::is_directory(status)) AddDisk(view,d,root,d.target,deferSharing);
        }
        return view;
    }
    fs::path Destination(const View& view,const Directory& d,std::string_view path) const {
        NativeName(Tail(d.target,path),path);
        auto disk=d.root; auto current=d.target;
        const auto relative=Tail(d.target,path);
        for(std::size_t start=0;start<relative.size();) {
            auto end=relative.find('/',start); if(end==std::string::npos) end=relative.size();
            const auto part=std::string_view(relative).substr(start,end-start);
            current=Join(current,part);
            if(const auto* node=view.Find(current);node&&node->directory==&d) disk=node->disk;
            else disk/=NativePath(part);
            start=end+1;
        }
        return disk;
    }
    void CheckNewName(const View& view,std::string_view path) const {
        const auto found=view.folded.find(FoldPath(path));
        if(found!=view.folded.end()&&found->second!=path) Fail("name_conflict","create",path);
    }
    void RequireParent(const View& view,const Directory& d,std::string_view path) const {
        const auto parent=Parent(path);
        const auto* node=view.Find(parent);
        if(!node) Fail("not_found","parent directory",path);
        if(node->info.type!=NodeType::Directory) Fail("not_directory","parent directory",path);
        if(node->directory!=&d||node->disk.empty()) Fail("not_found","real parent directory",path);
    }
    SDL_IOStream* OpenDisk(const View& view,const fs::path& disk,const OpenMode& mode,
        std::string_view path,const Node* node) const {
#ifdef _WIN32
        // Never resolve the verified parent path a second time in CreateFile.
        // An in-place reparse change can redirect an absolute open even while
        // its original directory handle remains alive with DELETE unshared.
        const auto parent=disk.parent_path();
        for(const auto& guard:view.guards) {
            if(guard.path!=parent) continue;
            const detail::NativeOpenTarget target{guard.anchor->handle,disk.filename(),
                node?node->identity:std::optional<FileIdentity>{},view.NeedsSingleLink(node?node->directory:&Route(path))};
            return detail::OpenNative(disk,mode,path,&target);
        }
        throw ConflictError("missing verified parent directory at '"+std::string(path)+"'");
#else
        (void)view; (void)node;
        return detail::OpenNative(disk,mode,path);
#endif
    }
    SDL_IOStream* OpenInView(const View& view,std::string_view p,const OpenMode& mode) const {
        view.RequireComplete(p);
        const auto* node=view.Resolve(p);
        view.RequireAliases(node);
        if(mode.exclusive&&node) Fail("already_exists","open exclusive",p);
        if(node&&node->info.type==NodeType::Directory) Fail("is_directory","open",p);
        if(node&&node->info.size>MaxFilePosition) Fail("out_of_range","open",p);
        if(mode.writable) {
            if(node&&node->mount) Fail("read_only","open",p);
            const auto& d=Route(p);
            if(mode.base=='r'&&!node) Fail("not_found","open",p);
            CheckNewName(view,p); RequireParent(view,d,p);
            const auto disk=Destination(view,d,p); CheckBacking(disk,p,false);
            return OpenDisk(view,disk,mode,p,node);
        }
        if(!node) Fail("not_found","open",p);
        if(node->mount) return node->mount->package->Open(node->relative);
        if(!node->directory) Fail("not_found","open",p);
        return OpenDisk(view,node->disk,mode,p,node);
    }
    void MutationSource(const View& view,const Node* node,std::string_view path,bool allowDirectory) const {
        view.RequireComplete(path,node&&node->info.type==NodeType::Directory);
        view.RequireAliases(node);
        if(Protected(path)) Fail("unsupported","modify mapping root",path);
        if(!node) Fail("not_found","modify",path);
        if(node->mount&&node->info.type==NodeType::File) Fail("read_only","modify",path);
        if(node->info.type==NodeType::Directory) {
            if(!allowDirectory) Fail("is_directory","modify",path);
            for(const auto& [name,child]:view.nodes) if(PathContains(path,name)&&child.mount)
                Fail("unsupported","modify merged directory",path);
            for(const auto& [name,child]:view.nodes)
                if(PathContains(path,name)&&child.directory&&child.info.type==NodeType::File)
                    CheckBacking(child.disk,path,false);
        }
        if(!node->directory||node->disk.empty()) Fail("read_only","modify",path);
        CheckBacking(node->disk,path,node->info.type==NodeType::Directory);
    }
    void CheckMovedAliases(const View& view,std::string_view source,std::string_view destination,const Directory& route) const {
        // Path-based relocation has no opened-file link-count recheck. Unlike
        // OpenNative, it cannot prove that a new alias was not added in another
        // pending mapping after the scan. Refuse before changing either object.
        if(view.NeedsSingleLink(&route))
            Fail("resource_busy","cannot verify relocation aliases while another mapping is pending",destination);
        std::set<FileIdentity> moved;
        for(const auto& [name,node]:view.nodes)
            if(PathContains(source,name)&&node.identity) {
                view.RequireAliases(&node,&route);
                moved.insert(*node.identity);
            }
        for(const auto& [name,node]:view.nodes)
            if(!PathContains(source,name)&&name!=destination&&node.identity&&node.directory!=&route&&moved.contains(*node.identity))
                Fail("name_conflict","move would authorize a physical alias",destination);
    }
};

FileSystem::FileSystem(fs::path installRoot,std::vector<MountSpec> specs,std::vector<DirectorySpec> rules,
    std::optional<std::array<std::uint8_t,32>> key,std::string_view initialEntry):impl_(std::make_unique<Impl>()) {
    VFS_PROFILE_SCOPE(Construct);
    impl_->install=BaseRoot(installRoot);
    if(specs.size()>4096) throw InvalidPath("too many compiled package mounts");
    Guards guards;
    std::sort(specs.begin(),specs.end(),[](const auto& a,const auto& b){return a.parts<b.parts;});
    for(std::size_t i=0;i<specs.size();++i) {
        const auto& spec=specs[i];
        if(i&&spec.parts==specs[i-1].parts) throw InvalidPath("package parts must be globally unique");
        const auto target=Strict(spec.target); const auto source=Strict(Utf8(spec.source));
        if(spec.source.is_absolute()||spec.source.has_root_path()) throw InvalidPath("package source must be deployment relative");
        if(spec.encrypted&&!key) throw InvalidPath("encrypted package requires a compiled key");
        const auto disk=ProjectPath(impl_->install,source,target,guards);
        if(!fs::is_regular_file(Status(disk,target))) Fail("not_found","mount required package",target);
        impl_->mounts.push_back({disk,target,spec.parts,spec.encrypted,Package::Load(disk,spec.encrypted,key)});
        impl_->protectedCode=impl_->protectedCode||spec.encrypted;
    }
    for(const auto& rule:rules) {
        const auto base=BaseRoot(rule.baseRoot);
        const bool deploymentRoot=rule.path.empty() && rule.target.empty();
        if((rule.path.empty() || rule.target.empty()) &&
            (!deploymentRoot || PhysicalKey(base)!=PhysicalKey(impl_->install)))
            throw InvalidPath("empty directory mappings are reserved for the deployment root");
        const auto path=deploymentRoot?std::string{}:Strict(rule.path);
        const auto target=deploymentRoot?std::string{}:Strict(rule.target);
        const auto root=ProjectPath(base,path,target,guards);
        Impl::Directory candidate{base,root,path,target,std::make_shared<DirectoryAnchor>(base,target),{}};
        bool redundant=false;
        for(auto it=impl_->directories.begin();it!=impl_->directories.end();) {
            const bool existingParent=PathContains(it->target,target),newParent=PathContains(target,it->target);
            const bool sameBase=PhysicalKey(it->base)==PhysicalKey(base);
            if(existingParent&&sameBase&&PhysicalKey(it->root/NativePath(Tail(it->target,target)))==PhysicalKey(root)) { redundant=true; break; }
            if(newParent&&sameBase&&PhysicalKey(root/NativePath(Tail(target,it->target)))==PhysicalKey(it->root)) { it=impl_->directories.erase(it); continue; }
            if(existingParent||newParent||PhysicalContains(root,it->root)||PhysicalContains(it->root,root))
                throw ConflictError("conflicting real directory mappings at '"+target+"'");
            ++it;
        }
        if(!redundant) impl_->directories.push_back(std::move(candidate));
    }
    // The initial entry check consumes this exact construction view. Release
    // its transient anchors here; the later Lua read must scan/open afresh.
    const auto view=impl_->Scan(false);
    if(!initialEntry.empty()) {
        const auto entry=impl_->Authorize(initialEntry);
        impl_->CheckCode(view,entry);
        const auto* node=view.Resolve(entry);
        if(!node||node->info.type!=NodeType::File)
            throw std::runtime_error("Compiled Lua entry is not an authorized file: "+entry);
    }
}
FileSystem::~FileSystem()=default;
std::optional<std::string> FileSystem::NormalizePath(std::string_view path) const { return impl_->Authorize(path); }
void FileSystem::CheckCodeSource(std::string_view path) const {
    VFS_PROFILE_SCOPE(CheckCodeSource);
    const auto p=impl_->Authorize(path); std::lock_guard lock(impl_->mutex); const auto view=impl_->Scan();
    view.RequireComplete(p);
    view.RequireAliases(view.Resolve(p));
    impl_->CheckCode(view,p);
}
NodeInfo FileSystem::Stat(std::string_view path,bool codeSource) const {
    VFS_PROFILE_SCOPE(Stat);
    const auto p=impl_->Authorize(path); std::lock_guard lock(impl_->mutex); const auto view=impl_->Scan();
    view.RequireComplete(p);
    if(codeSource) impl_->CheckCode(view,p);
    const auto* node=view.Resolve(p); view.RequireAliases(node); return node?node->info:NodeInfo{};
}
std::optional<std::vector<std::string>> FileSystem::ListDirectory(std::string_view path) const {
    VFS_PROFILE_SCOPE(ListDirectory);
    const auto p=impl_->Authorize(path); std::lock_guard lock(impl_->mutex); const auto view=impl_->Scan();
    view.RequireComplete(p);
    const auto* node=view.Resolve(p);
    if(!node) Fail("not_found","list",p);
    if(node->info.type!=NodeType::Directory) Fail("not_directory","list",p);
    std::vector<std::string> result; const auto prefix=p+'/';
    for(auto it=view.nodes.lower_bound(prefix);it!=view.nodes.end()&&it->first.starts_with(prefix);++it) {
        const auto tail=std::string_view(it->first).substr(prefix.size());
        if(tail.find('/')==std::string_view::npos) {
            view.RequireAliases(&it->second);
            result.emplace_back(tail);
        }
    }
    return result;
}
SDL_IOStream* FileSystem::Open(std::string_view path,std::string_view mode) const { return Open(path,ParseMode(mode)); }
std::shared_ptr<const ReadSource> FileSystem::OpenReadSource(std::string_view path) const {
    VFS_PROFILE_SCOPE(OpenReadSource);
    const auto name=impl_->Authorize(path);
    std::shared_ptr<SDL_IOStream> stream(Open(name,"rb"),[](SDL_IOStream* value){ SDL_CloseIO(value); });
    return std::shared_ptr<const ReadSource>(new ReadSource(std::move(stream),name));
}
SDL_IOStream* FileSystem::Open(std::string_view path,const OpenMode& mode) const {
    return OpenValidated(path,mode,false);
}
SDL_IOStream* FileSystem::OpenValidated(std::string_view path,const OpenMode& mode,bool codeSource) const {
    VFS_PROFILE_SCOPE(Open);
    const auto p=impl_->Authorize(path); std::lock_guard lock(impl_->mutex);
    std::optional<std::chrono::steady_clock::time_point> retryDeadline;
    for(unsigned attempt=0;attempt!=8;++attempt) {
        const auto view=impl_->Scan();
        if(codeSource) impl_->CheckCode(view,p);
        try { return impl_->OpenInView(view,p,mode); }
        catch(const detail::NativeViewChanged&) {
            // A replaced leaf must be re-authorized, never truncated via the
            // previous identity. Allow a short burst of atomic replacements to
            // settle. Always permit one revalidation, but do not perform eight
            // expensive full scans of a large, continuously changing tree.
            const auto now=std::chrono::steady_clock::now();
            if(!retryDeadline) retryDeadline=now+std::chrono::milliseconds(25);
            else if(now>=*retryDeadline) break;
        }
    }
    Fail("resource_busy","filesystem changed while opening",p);
}
bool FileSystem::CreateDirectory(std::string_view path) const {
    VFS_PROFILE_SCOPE(CreateDirectory);
    const auto p=impl_->Authorize(path); const auto& d=impl_->Route(p);
    std::lock_guard lock(impl_->mutex); auto view=impl_->Scan();
    view.RequireComplete(p);
    if(const auto* node=view.Find(p);node&&node->info.type==NodeType::File) Fail("already_exists","create directory",p);
    impl_->CheckNewName(view,p);
    const auto disk=impl_->Destination(view,d,p); impl_->CheckBacking(disk,p,true);
    // Create each fixed project ancestor explicitly, never an arbitrary base.
    auto current=d.base;
    const auto combined=Join(d.path,Tail(d.target,p));
    for(std::size_t start=0;start<combined.size();) {
        auto end=combined.find('/',start); if(end==std::string::npos) end=combined.size();
        const auto part=std::string_view(combined).substr(start,end-start);
        Guard(current,p,view.guards);
        if(const auto existing=Child(current,part,p)) current=*existing;
        else {
            // A differently cased name must never become an OS-dependent alias.
            VFS_PROFILE_ADD(DirectoryEnumerations,1);
            std::error_code ec; fs::directory_iterator it(current,ec),last; Check(ec,"create directory",p);
            while(it!=last) {
                VFS_PROFILE_ADD(EntriesEnumerated,1);
                const auto name=Normalize(Utf8(it->path().filename()));
                if(name&&FoldPath(*name)==FoldPath(part)) Fail("name_conflict","create directory",p);
                it.increment(ec); Check(ec,"create directory",p);
            }
            current/=NativePath(part);
            fs::create_directory(current,ec); Check(ec,"create directory",p);
        }
        if(!fs::is_directory(Status(current,p))) Fail("not_directory","create directory",p);
        start=end+1;
    }
    (void)impl_->Scan(); return true;
}
bool FileSystem::Remove(std::string_view path) const {
    VFS_PROFILE_SCOPE(Remove);
    const auto p=impl_->Authorize(path); std::lock_guard lock(impl_->mutex); auto view=impl_->Scan();
    const auto* node=view.Find(p); impl_->MutationSource(view,node,p,true);
    if(node->info.type==NodeType::Directory) {
        const auto prefix=p+'/'; const auto next=view.nodes.lower_bound(prefix);
        if(next!=view.nodes.end()&&next->first.starts_with(prefix)) Fail("directory_not_empty","remove",p);
        view.UnlockSubtree(node->disk);
    }
    std::error_code ec; const auto removed=fs::remove(node->disk,ec); Check(ec,"remove",p);
    if(!removed) Fail("not_found","remove",p);
    (void)impl_->Scan(); return true;
}
bool FileSystem::Rename(std::string_view directory,std::string_view oldName,std::string_view newName) const {
    VFS_PROFILE_SCOPE(Rename);
    const auto dir=impl_->Authorize(directory),a=Strict(oldName),b=Strict(newName);
    if(a.find('/')!=std::string::npos||b.find('/')!=std::string::npos) throw InvalidPath("Rename requires two single filenames");
    const auto source=dir+'/'+a,destination=dir+'/'+b;
    (void)impl_->Authorize(source); (void)impl_->Authorize(destination);
    return MoveValidated(source,destination,false);
}
bool FileSystem::Copy(std::string_view source,std::string_view destination) const {
    VFS_PROFILE_SCOPE(Copy);
    const auto a=impl_->Authorize(source),b=impl_->Authorize(destination);
    std::lock_guard lock(impl_->mutex); const auto view=impl_->Scan();
    view.RequireComplete(a); view.RequireComplete(b);
    const auto* from=view.Resolve(a); const auto* to=view.Resolve(b);
    view.RequireAliases(from); view.RequireAliases(to);
    if(!from) Fail("not_found","copy",a);
    if(from->info.type!=NodeType::File) Fail("is_directory","copy",a);
    if(to) Fail("already_exists","copy",b);
    const auto& d=impl_->Route(b); impl_->CheckNewName(view,b); impl_->RequireParent(view,d,b);
    impl_->CheckBacking(impl_->Destination(view,d,b),b,false);
    Stream input,output;
    try {
        // The full view, including both parent anchors, is already validated.
        // Opening either stream still verifies the actual file generation.
        input.reset(impl_->OpenInView(view,a,ParseMode("rb")));
        output.reset(impl_->OpenInView(view,b,ParseMode("xb")));
    } catch(const detail::NativeViewChanged&) {
        Fail("resource_busy","filesystem changed while copying",a);
    }
    std::exception_ptr failure;
    try {
        std::array<char,64*1024> bytes{};
        for(;;) {
            const auto count=SDL_ReadIO(input.get(),bytes.data(),bytes.size()); const auto state=SDL_GetIOStatus(input.get());
            if(state==SDL_IO_STATUS_ERROR) throw GetStreamError(input.get(),"copy read",a);
            if(count&&SDL_WriteIO(output.get(),bytes.data(),count)!=count) throw GetStreamError(output.get(),"copy write",b);
            if(state==SDL_IO_STATUS_EOF) break;
            if(!count) Fail("io_error","copy read",a);
        }
    } catch(...) { failure=std::current_exception(); }
    try { CloseStream(output.release(),b); } catch(...) { if(!failure) failure=std::current_exception(); }
    try { CloseStream(input.release(),a); } catch(...) { if(!failure) failure=std::current_exception(); }
    if(failure) std::rethrow_exception(failure);
    (void)impl_->Scan(); return true;
}
bool FileSystem::Move(std::string_view source,std::string_view destination) const {
    VFS_PROFILE_SCOPE(Move);
    return MoveValidated(source,destination,true);
}
bool FileSystem::MoveValidated(std::string_view source,std::string_view destination,bool allowDirectory) const {
    const auto a=impl_->Authorize(source),b=impl_->Authorize(destination);
    std::lock_guard lock(impl_->mutex); auto view=impl_->Scan(); const auto* from=view.Resolve(a);
    view.RequireComplete(a); view.RequireComplete(b);
    if(!allowDirectory&&from&&from->info.type==NodeType::Directory) Fail("is_directory","rename",a);
    impl_->MutationSource(view,from,a,true);
    if(view.Resolve(b)) Fail("already_exists","move",b);
    const auto& d=impl_->Route(b); impl_->CheckNewName(view,b); impl_->RequireParent(view,d,b);
    if(PathContains(a,b)) Fail("unsupported","move into own subtree",b);
    const auto disk=impl_->Destination(view,d,b); impl_->CheckBacking(disk,b,true);
    impl_->CheckMovedAliases(view,a,b,d);
    view.UnlockSubtree(from->disk);
    RenamePhysical(from->disk,disk,false,a);
    (void)impl_->Scan(); return true;
}
bool FileSystem::Replace(std::string_view source,std::string_view destination) const {
    VFS_PROFILE_SCOPE(Replace);
    const auto a=impl_->Authorize(source),b=impl_->Authorize(destination);
    std::lock_guard lock(impl_->mutex); auto view=impl_->Scan(); const auto* from=view.Resolve(a); const auto* to=view.Resolve(b);
    view.RequireComplete(a); view.RequireComplete(b); view.RequireAliases(to);
    impl_->MutationSource(view,from,a,false);
    if(impl_->Protected(b)) Fail("unsupported","replace mapping root",b);
    if(to&&to->info.type==NodeType::Directory) Fail("is_directory","replace",b);
    if(to&&to->mount) Fail("read_only","replace",b);
    const auto& d=impl_->Route(b); impl_->CheckNewName(view,b); impl_->RequireParent(view,d,b);
    const auto disk=impl_->Destination(view,d,b); impl_->CheckBacking(disk,b,false);
    impl_->CheckMovedAliases(view,a,b,d);
    if(a==b) return true;
    RenamePhysical(from->disk,disk,true,a);
    (void)impl_->Scan(); return true;
}
Snapshot FileSystem::ReadSnapshot(std::string_view path,const ReadOptions& options,bool codeSource) const {
    VFS_PROFILE_SCOPE(ReadSnapshot);
    if(!options.maxBytes||!options.chunkBytes||options.chunkBytes>16*1024*1024||options.prefixBytes>4096)
        throw std::invalid_argument("invalid VFS snapshot read limits");
    const auto p=impl_->Authorize(path); Cancel(options);
    Stream stream(OpenValidated(p,ParseMode("rb"),codeSource));
    const auto modified=detail::NativeModified(stream.get(),p);
    const auto signedSize=SDL_GetIOSize(stream.get());
    if(signedSize<0) throw GetStreamError(stream.get(),"snapshot size",p);
    const auto size=static_cast<std::uint64_t>(signedSize);
    if(size>options.maxBytes||size>std::string{}.max_size()) Fail("out_of_range","snapshot size",p);
    const auto exact=[&](char* out,std::size_t wanted) {
        while(wanted) {
            Cancel(options); const auto request=std::min(wanted,options.chunkBytes);
            const auto count=SDL_ReadIO(stream.get(),out,request);
            if(SDL_GetIOStatus(stream.get())==SDL_IO_STATUS_ERROR) throw GetStreamError(stream.get(),"snapshot read",p);
            if(!count||count>request) Fail("resource_busy","file changed during snapshot",p);
            out+=count; wanted-=count;
        }
        Cancel(options);
    };
    std::array<char,4096> prefix{}; const auto prefixSize=static_cast<std::size_t>(std::min<std::uint64_t>(size,options.prefixBytes));
    exact(prefix.data(),prefixSize);
    if(options.admit) options.admit(size,{prefix.data(),prefixSize});
    Cancel(options);
    Snapshot result; result.bytes.resize(static_cast<std::size_t>(size));
    std::copy_n(prefix.data(),prefixSize,result.bytes.data()); exact(result.bytes.data()+prefixSize,result.bytes.size()-prefixSize);
    char extra{}; const auto more=SDL_ReadIO(stream.get(),&extra,1);
    if(SDL_GetIOStatus(stream.get())==SDL_IO_STATUS_ERROR) throw GetStreamError(stream.get(),"snapshot finish",p);
    const auto afterSize=SDL_GetIOSize(stream.get());
    if(afterSize<0) throw GetStreamError(stream.get(),"snapshot size",p);
    if(more||afterSize!=signedSize||detail::NativeModified(stream.get(),p)!=modified)
        Fail("resource_busy","file changed during snapshot",p);
    result.modified=modified.value_or(0);
    CloseStream(stream.release(),p); Cancel(options);
    return result;
}
std::optional<std::string> FileSystem::Read(std::string_view path,bool codeSource) const {
    ReadOptions options; options.maxBytes=std::string{}.max_size();
    return ReadSnapshot(path,options,codeSource).bytes;
}
}
