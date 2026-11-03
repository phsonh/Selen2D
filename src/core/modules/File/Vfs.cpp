#include "Vfs.h"
#include "Package.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef CreateDirectory
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif

namespace core::modules::File {
namespace {
namespace fs = std::filesystem;
std::string Utf8(const fs::path& p) {
    const auto s=p.generic_u8string(); return {reinterpret_cast<const char*>(s.data()),s.size()};
}
bool Utf8Valid(std::string_view s) {
    for(std::size_t i=0;i<s.size();) {
        const auto first=static_cast<unsigned char>(s[i++]);
        if(first<0x80) continue;
        unsigned count{}, value{}, minimum{};
        if(first>=0xc2 && first<=0xdf) { count=1; value=first&31; minimum=0x80; }
        else if(first>=0xe0 && first<=0xef) { count=2; value=first&15; minimum=0x800; }
        else if(first>=0xf0 && first<=0xf4) { count=3; value=first&7; minimum=0x10000; }
        else return false;
        if(count>s.size()-i) return false;
        while(count--) { const auto next=static_cast<unsigned char>(s[i++]); if((next&0xc0)!=0x80) return false; value=(value<<6)|(next&63); }
        if(value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) return false;
    }
    return true;
}
bool Component(std::string_view c) {
    if(c.empty() || c=="." || c==".." || c.back()=='.' || c.back()==' ') return false;
    for(const unsigned char ch:c) if(ch<32 || ch==127 || std::string_view(":*?\"<>|/\\").find(ch)!=std::string_view::npos) return false;
    // Reject Windows device aliases on every platform so packages are portable.
    std::string stem(c.substr(0,c.find('.')));
    for(auto& ch:stem) if(ch>='a' && ch<='z') ch-=32;
    if(stem=="CON" || stem=="PRN" || stem=="AUX" || stem=="NUL" || stem=="CONIN$" || stem=="CONOUT$") return false;
    if(stem.size()==4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3]>='1' && stem[3]<='9') return false;
    return true;
}
bool Within(std::string_view parent,std::string_view child) {
    return parent.empty() || parent==child || (child.size()>parent.size() && child.starts_with(parent) && child[parent.size()]=='/');
}
bool UnsafeLink(const fs::path& p) {
    std::error_code ec;
    const auto status=fs::symlink_status(p,ec);
    if(ec && ec!=std::errc::no_such_file_or_directory) return true;
    if(fs::is_symlink(status)) return true;
#ifdef _WIN32
    const auto attributes=GetFileAttributesW(p.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return false;
}
fs::path DiskSpelling(const fs::path& path) {
#ifdef _WIN32
    // canonical() does not expand Windows 8.3 aliases. Compare the long form
    // so alternate spellings cannot bypass protection of a mounted source.
    const auto length=GetLongPathNameW(path.c_str(),nullptr,0);
    if(length) {
        std::wstring text(length,L'\0');
        const auto written=GetLongPathNameW(path.c_str(),text.data(),length);
        if(written && written<length) { text.resize(written); return fs::path(text); }
    }
#endif
    return path;
}
// This prevents ordinary symlink/junction escapes. It is not a hostile-process
// filesystem sandbox: external replacement between validation and open is out
// of scope. Every operation rechecks the path rather than caching permission.
std::optional<fs::path> DiskPath(const fs::path& root,std::string_view relative) {
    fs::path p=DiskSpelling(root);
    if(UnsafeLink(p)) return {};
    for(const auto& part:fs::u8path(relative)) {
        p/=part;
        if(UnsafeLink(p)) return {};
        p=DiskSpelling(p);
    }
    return p;
}
NodeInfo DiskStat(const fs::path& p) {
    SDL_PathInfo info{};
    if(!SDL_GetPathInfo(Utf8(p).c_str(),&info)) return {};
    if(info.type==SDL_PATHTYPE_DIRECTORY) return {NodeType::Directory};
    if(info.type==SDL_PATHTYPE_FILE) return {NodeType::File,info.size,static_cast<std::uint64_t>(info.modify_time)};
    return {};
}
std::optional<std::vector<std::string>> DiskList(const fs::path& p) {
    std::error_code ec;
    fs::directory_iterator it(p,ec),end;
    if(ec) return {};
    std::vector<std::string> result;
    while(it!=end) {
        const auto name=Utf8(it->path().filename());
        if(Component(name) && Utf8Valid(name) && !UnsafeLink(it->path())) result.push_back(name);
        it.increment(ec); if(ec) return {};
    }
    return result;
}
struct Closer { void operator()(SDL_IOStream* s) const { if(s) SDL_CloseIO(s); } };
using Stream=std::unique_ptr<SDL_IOStream,Closer>;
bool RenameNoReplace(const fs::path& source,const fs::path& destination) {
#ifdef _WIN32
    // MOVEFILE_COPY_ALLOWED deliberately absent: a failed move retains source.
    return MoveFileExW(source.c_str(),destination.c_str(),0)!=0;
#elif defined(__linux__) && defined(SYS_renameat2)
    return syscall(SYS_renameat2,AT_FDCWD,source.c_str(),AT_FDCWD,destination.c_str(),1 /* RENAME_NOREPLACE */)==0;
#elif defined(__APPLE__)
    return renamex_np(source.c_str(),destination.c_str(),RENAME_EXCL)==0;
#else
    // A preflight exists() + overwriting rename is racy. Unsupported platforms
    // fail instead of violating the no-overwrite contract.
    (void)source; (void)destination; return false;
#endif
}
void Cancel(const ReadOptions& o) { if(o.cancelled && o.cancelled()) throw ReadCancelled(); }
}

std::optional<std::string> Normalize(std::string_view input) {
    if(input.size()>4096 || !Utf8Valid(input) || input.find('\0')!=std::string_view::npos) return {};
    std::string value(input); std::replace(value.begin(),value.end(),'\\','/');
    if(!value.empty() && value.front()=='/') return {};
    std::vector<std::string_view> parts;
    std::size_t begin{};
    while(begin<value.size()) {
        auto end=value.find('/',begin); if(end==std::string::npos) end=value.size();
        const std::string_view part(value.data()+begin,end-begin); begin=end+1;
        if(part.empty() || part==".") continue;
        if(part=="..") {
            if(parts.empty()) return {};
            parts.pop_back();
        } else { if(!Component(part)) return {}; parts.push_back(part); }
    }
    std::string result;
    for(const auto part:parts) { if(!result.empty()) result+='/'; result+=part; }
    return result;
}
OpenMode ParseMode(std::string_view mode) {
    if(mode.empty() || std::string_view("rwax").find(mode.front())==std::string_view::npos)
        throw std::invalid_argument("File.Open mode must start with r, w, a or x");
    bool binary{},plus{};
    for(std::size_t i=1;i<mode.size();++i) {
        if(mode[i]=='b' && !binary) binary=true;
        else if(mode[i]=='+' && !plus) plus=true;
        else throw std::invalid_argument("invalid File.Open mode");
    }
    OpenMode m; m.base=mode.front(); m.readable=m.base=='r'||plus;
    m.writable=m.base!='r'||plus; m.append=m.base=='a'; m.exclusive=m.base=='x';
    m.sdlMode=std::string(1,m.exclusive?'w':m.base)+"b"+(plus?"+":"")+(m.exclusive?"x":"");
    return m;
}

struct FileSystem::Impl {
    struct Mount { std::string target; fs::path directory; std::shared_ptr<Package> package; };
    struct Resolved { NodeInfo info; fs::path disk; std::shared_ptr<Package> package; std::string relative; };
    fs::path root;
    std::vector<Mount> mounts;
    bool Mounted(std::string_view path) const {
        return std::any_of(mounts.begin(),mounts.end(),[&](const Mount& m){ return Within(m.target,path); });
    }
    bool Protected(std::string_view path) const {
        return path.empty() || std::any_of(mounts.begin(),mounts.end(),[&](const Mount& m){
            return Within(m.target,path) || Within(path,m.target);
        });
    }
    Resolved Raw(std::string_view path) const {
        if(path.empty()) return {{NodeType::Directory},{},{},{}};
        for(const auto& mount:mounts)
            if(path==mount.target || Within(path,mount.target)) return {{NodeType::Directory},{},{},{}};
        for(auto it=mounts.rbegin();it!=mounts.rend();++it) {
            if(!Within(it->target,path)) continue;
            const auto relative=std::string(path.substr(it->target.empty()?0:it->target.size()+1));
            if(it->package) {
                const auto info=it->package->Stat(relative);
                if(info.type!=NodeType::Missing) return {info,{},it->package,relative};
            } else if(const auto disk=DiskPath(it->directory,relative)) {
                const auto info=DiskStat(*disk);
                if(info.type!=NodeType::Missing) return {info,*disk,{},{}};
            }
        }
        // Mounted names never fall through to an unrelated physical file.
        if(Mounted(path)) return {};
        const auto disk=DiskPath(root,path);
        return disk ? Resolved{DiskStat(*disk),*disk,{},{}} : Resolved{};
    }
    Resolved Resolve(std::string_view p) const {
        for(auto slash=p.find('/');slash!=std::string_view::npos;slash=p.find('/',slash+1))
            if(Raw(p.substr(0,slash)).info.type!=NodeType::Directory) return {};
        return Raw(p);
    }
    std::optional<fs::path> WritePath(std::string_view p) const {
        if(Protected(p)) return {};
        const auto result=DiskPath(root,p); if(!result) return {};
        // A mounted archive/directory may also be addressable through the disk
        // root. Protect that physical alias, including ancestor moves/removals.
        for(const auto& mount:mounts) {
            const auto& backing=mount.directory;
            if(backing.empty()) continue;
            std::error_code ec;
            if(mount.package && fs::equivalent(*result,backing,ec) && !ec) return {};
            auto same=[](const fs::path& a,const fs::path& b) {
#ifdef _WIN32
                return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_EQUAL;
#else
                return a==b;
#endif
            };
            auto prefix=[&](const fs::path& a,const fs::path& b) {
                auto x=a.begin(),y=b.begin();
                for(;x!=a.end();++x,++y) if(y==b.end() || !same(*x,*y)) return false;
                return true;
            };
            if(prefix(*result,backing) || (mount.package ? same(*result,backing) : prefix(backing,*result))) return {};
        }
        return result;
    }
};
FileSystem::FileSystem(fs::path installRoot,std::vector<MountSpec> specs) : impl_(std::make_unique<Impl>()) {
    std::error_code ec;
    impl_->root=DiskSpelling(fs::canonical(installRoot,ec));
    if(ec || !fs::is_directory(impl_->root)) throw std::invalid_argument("VFS install root must be an existing directory");
    for(const auto& spec:specs) {
        const auto target=Normalize(spec.target);
        if(!target) throw std::invalid_argument("invalid VFS mount target");
        const auto source=DiskSpelling(fs::canonical(impl_->root/spec.source,ec));
        if(ec) throw std::runtime_error("cannot resolve VFS mount: "+Utf8(spec.source));
        Impl::Mount mount; mount.target=*target; mount.directory=source;
        if(!fs::is_directory(source,ec)) {
            if(!ec && fs::is_regular_file(source,ec)) mount.package=Package::Load(source);
            else throw std::runtime_error("VFS mount is neither directory nor package: "+Utf8(source));
        }
        impl_->mounts.push_back(std::move(mount));
    }
}
FileSystem::~FileSystem()=default;
std::optional<std::string> FileSystem::NormalizePath(std::string_view path) const {
    const auto normalized=Normalize(path); if(!normalized) return {};
    std::string raw(path); std::replace(raw.begin(),raw.end(),'\\','/');
    std::string prefix;
    for(std::size_t begin=0;begin<raw.size();) {
        auto end=raw.find('/',begin); if(end==std::string::npos) end=raw.size();
        const auto part=std::string_view(raw).substr(begin,end-begin); begin=end+1;
        if(part.empty() || part==".") continue;
        if(part=="..") {
            for(const auto& mount:impl_->mounts) if(prefix==mount.target) return {};
            const auto slash=prefix.rfind('/'); prefix=slash==std::string::npos?"":prefix.substr(0,slash);
        } else { if(!prefix.empty()) prefix+='/'; prefix+=part; }
    }
    return normalized;
}
NodeInfo FileSystem::Stat(std::string_view path) const {
    const auto p=NormalizePath(path); return p?impl_->Resolve(*p).info:NodeInfo{};
}
std::optional<std::vector<std::string>> FileSystem::ListDirectory(std::string_view path) const {
    const auto p=NormalizePath(path);
    if(!p || impl_->Resolve(*p).info.type!=NodeType::Directory) return {};
    std::set<std::string> names;
    if(!impl_->Mounted(*p)) {
        const auto disk=DiskPath(impl_->root,*p);
        if(disk && DiskStat(*disk).type==NodeType::Directory) {
            auto list=DiskList(*disk); if(!list) return {};
            names.insert(list->begin(),list->end());
        }
    }
    for(const auto& mount:impl_->mounts) {
        if(*p!=mount.target && Within(*p,mount.target)) {
            auto tail=std::string_view(mount.target).substr(p->empty()?0:p->size()+1);
            names.emplace(tail.substr(0,tail.find('/')));
        } else if(Within(mount.target,*p)) {
            const auto relative=p->substr(mount.target.empty()?0:(*p==mount.target?p->size():mount.target.size()+1));
            std::optional<std::vector<std::string>> list;
            if(mount.package) {
                if(mount.package->Stat(relative).type!=NodeType::Directory) continue;
                list=mount.package->ListDirectory(relative);
            } else {
                const auto disk=DiskPath(mount.directory,relative);
                if(!disk || DiskStat(*disk).type!=NodeType::Directory) continue;
                list=DiskList(*disk);
            }
            if(!list) return {};
            names.insert(list->begin(),list->end());
        }
    }
    std::vector<std::string> result;
    for(const auto& name:names) {
        const auto child=p->empty()?name:*p+"/"+name;
        if(impl_->Resolve(child).info.type!=NodeType::Missing) result.push_back(name);
    }
    return result;
}
SDL_IOStream* FileSystem::Open(std::string_view path,std::string_view mode) const { return Open(path,ParseMode(mode)); }
SDL_IOStream* FileSystem::Open(std::string_view path,const OpenMode& mode) const {
    const auto p=NormalizePath(path);
    if(!p || p->empty()) { SDL_SetError("invalid virtual file path"); return nullptr; }
    if(mode.writable) {
        const auto disk=impl_->WritePath(*p);
        if(!disk) { SDL_SetError("virtual path is read-only or not a file"); return nullptr; }
        return SDL_IOFromFile(Utf8(*disk).c_str(),mode.sdlMode.c_str());
    }
    const auto resolved=impl_->Resolve(*p);
    if(resolved.info.type!=NodeType::File) { SDL_SetError("virtual file not found: %s",p->c_str()); return nullptr; }
    if(resolved.package) return resolved.package->Open(resolved.relative);
    return SDL_IOFromFile(Utf8(resolved.disk).c_str(),"rb");
}
bool FileSystem::CreateDirectory(std::string_view path) const {
    const auto p=NormalizePath(path); if(!p) return false;
    if(p->empty()) return !impl_->Mounted(*p);
    const auto disk=impl_->WritePath(*p); if(!disk) return false;
    std::error_code ec; fs::create_directories(*disk,ec); return !ec;
}
bool FileSystem::Remove(std::string_view path) const {
    const auto p=NormalizePath(path); if(!p) return false;
    const auto disk=impl_->WritePath(*p); if(!disk) return false;
    std::error_code ec; return fs::remove(*disk,ec) && !ec;
}
bool FileSystem::Rename(std::string_view directory,std::string_view oldName,std::string_view newName) const {
    if(!Component(oldName) || !Component(newName) || !Utf8Valid(oldName) || !Utf8Valid(newName)) return false;
    const auto dir=NormalizePath(directory); if(!dir) return false;
    const auto prefix=dir->empty()?"":*dir+"/";
    const auto from=prefix+std::string(oldName),to=prefix+std::string(newName);
    return Stat(from).type==NodeType::File && Move(from,to);
}
bool FileSystem::Copy(std::string_view source,std::string_view destination) const {
    const auto dest=NormalizePath(destination); if(!dest || !impl_->WritePath(*dest)) return false;
    Stream input(Open(source,"rb")); if(!input) return false;
    Stream output(Open(*dest,"xb")); if(!output) return false;
    const auto disk=impl_->WritePath(*dest);
    bool ok=true;
    std::array<char,64*1024> bytes;
    for(;;) {
        const auto count=SDL_ReadIO(input.get(),bytes.data(),bytes.size());
        const auto status=SDL_GetIOStatus(input.get());
        if(status==SDL_IO_STATUS_ERROR || (count && SDL_WriteIO(output.get(),bytes.data(),count)!=count)) { ok=false; break; }
        if(status==SDL_IO_STATUS_EOF) break;
        if(!count) { ok=false; break; }
    }
    const bool flushed=SDL_FlushIO(output.get());
    const bool closed=SDL_CloseIO(output.release());
    const bool readClosed=SDL_CloseIO(input.release());
    ok=ok && flushed && closed && readClosed;
    if(!ok && disk) { std::error_code ec; fs::remove(*disk,ec); }
    return ok;
}
bool FileSystem::Move(std::string_view source,std::string_view destination) const {
    const auto a=NormalizePath(source),b=NormalizePath(destination);
    if(!a || !b) return false;
    const auto from=impl_->WritePath(*a),to=impl_->WritePath(*b);
    if(!from || !to || Stat(*a).type==NodeType::Missing || Stat(*b).type!=NodeType::Missing) return false;
    return RenameNoReplace(*from,*to);
}
bool FileSystem::Replace(std::string_view source,std::string_view destination) const {
    const auto a=NormalizePath(source),b=NormalizePath(destination); if(!a || !b) return false;
    const auto from=impl_->WritePath(*a),to=impl_->WritePath(*b);
    if(!from || !to || Stat(*a).type!=NodeType::File || Stat(*b).type==NodeType::Directory) return false;
    // SDL maps to MoveFileEx(REPLACE_EXISTING) / rename; no delete-first and no
    // cross-volume copy fallback. Failed replacement preserves the old target.
    return SDL_RenamePath(Utf8(*from).c_str(),Utf8(*to).c_str());
}
Snapshot FileSystem::ReadSnapshot(std::string_view path,const ReadOptions& options) const {
    if(!options.maxBytes || !options.chunkBytes || options.chunkBytes>16*1024*1024 || options.prefixBytes>4096)
        throw std::invalid_argument("invalid VFS snapshot read limits");
    Cancel(options);
    const auto p=NormalizePath(path); if(!p) throw std::runtime_error("invalid VFS snapshot path");
    const auto resolved=impl_->Resolve(*p);
    if(resolved.info.type!=NodeType::File) throw std::runtime_error("VFS file not found: "+*p);
    if(!resolved.package) return File::ReadSnapshot(Utf8(resolved.disk),options);
    const auto size=resolved.info.size;
    if(size>options.maxBytes || size>std::string{}.max_size()) throw std::length_error("VFS snapshot exceeds byte limit: "+*p);
    Stream stream(resolved.package->Open(resolved.relative));
    if(!stream) throw std::runtime_error(std::string("open VFS snapshot: ")+SDL_GetError());
    const auto exact=[&](char* out,std::size_t wanted) {
        while(wanted) {
            Cancel(options);
            const auto n=SDL_ReadIO(stream.get(),out,std::min(wanted,options.chunkBytes));
            if(SDL_GetIOStatus(stream.get())==SDL_IO_STATUS_ERROR || !n)
                throw std::runtime_error(std::string("read VFS snapshot: ")+SDL_GetError());
            out+=n; wanted-=n;
        }
        Cancel(options);
    };
    std::array<char,4096> prefix;
    const auto prefixSize=std::min<std::size_t>(static_cast<std::size_t>(size),options.prefixBytes);
    exact(prefix.data(),prefixSize);
    if(options.admit) options.admit(size,{prefix.data(),prefixSize});
    Cancel(options);
    Snapshot result; result.bytes.resize(static_cast<std::size_t>(size));
    std::copy_n(prefix.data(),prefixSize,result.bytes.data());
    exact(result.bytes.data()+prefixSize,result.bytes.size()-prefixSize);
    if(!SDL_CloseIO(stream.release())) throw std::runtime_error(std::string("close VFS snapshot: ")+SDL_GetError());
    Cancel(options); result.modified=resolved.info.modified; return result;
}
std::optional<std::string> FileSystem::Read(std::string_view path) const {
    ReadOptions options; options.maxBytes=std::string{}.max_size();
    try { return ReadSnapshot(path,options).bytes; }
    catch(const std::runtime_error&) { return {}; }
}
}
