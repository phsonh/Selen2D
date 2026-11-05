#include "NativeIo.h"
#include "Vfs.h"
#include "Profile.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winternl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace core::modules::File::detail {
namespace {
constexpr const char* NativeStateProperty="engine.io.native-state";
struct NativeStream {
#ifdef _WIN32
    HANDLE file{INVALID_HANDLE_VALUE};
#else
    int file{-1};
#endif
    OpenMode mode;
    std::uint64_t position{};
    std::shared_ptr<StreamErrorState> error{std::make_shared<StreamErrorState>()};
    NativeStream() { error->code.reserve(64); }
    ~NativeStream() {
#ifdef _WIN32
        if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
#else
        if(file>=0) ::close(file);
#endif
    }
    void Fail(std::string_view code) noexcept {
        try { error->code.assign(code); } catch(...) {}
        SDL_SetError("File I/O failed: %.*s",static_cast<int>(code.size()),code.data());
    }
    void SystemFailure() noexcept {
#ifdef _WIN32
        const auto value=GetLastError();
#else
        const auto value=errno;
#endif
        try { Fail(IoErrorCode({static_cast<int>(value),std::system_category()})); }
        catch(...) { Fail("io_error"); }
    }
    Sint64 Size() noexcept {
        VFS_PROFILE_SCOPE(NativeSize);
#ifdef _WIN32
        LARGE_INTEGER length{};
        VFS_PROFILE_ADD(AttributeQueries,1);
        if(!GetFileSizeEx(file,&length)) { SystemFailure(); return -1; }
        const auto size=static_cast<std::uint64_t>(length.QuadPart);
#else
        struct stat info{};
        VFS_PROFILE_ADD(AttributeQueries,1);
        if(fstat(file,&info)) { SystemFailure(); return -1; }
        const auto size=static_cast<std::uint64_t>(info.st_size);
#endif
        if(size>MaxFilePosition) { Fail("out_of_range"); return -1; }
        return static_cast<Sint64>(size);
    }
    bool At(std::uint64_t offset) noexcept {
#ifdef _WIN32
        LARGE_INTEGER at{}; at.QuadPart=static_cast<LONGLONG>(offset);
        if(!SetFilePointerEx(file,at,nullptr,FILE_BEGIN)) { SystemFailure(); return false; }
#else
        if(lseek(file,static_cast<off_t>(offset),SEEK_SET)<0) { SystemFailure(); return false; }
#endif
        return true;
    }
    std::size_t WriteSome(const void* data,std::size_t size) noexcept {
#ifdef _WIN32
        DWORD count{};
        if(!WriteFile(file,data,static_cast<DWORD>(std::min<std::size_t>(size,0x7fffffff)),&count,nullptr)) { SystemFailure(); return 0; }
        return count;
#else
        const auto count=::write(file,data,std::min<std::size_t>(size,0x7fffffff));
        if(count<0) { SystemFailure(); return 0; }
        return static_cast<std::size_t>(count);
#endif
    }
    static Sint64 SDLCALL GetSize(void* value) { return static_cast<NativeStream*>(value)->Size(); }
    static Sint64 SDLCALL Seek(void* value,Sint64 offset,SDL_IOWhence whence) {
        auto& s=*static_cast<NativeStream*>(value);
        std::uint64_t base{};
        if(whence==SDL_IO_SEEK_CUR) base=s.position;
        else if(whence==SDL_IO_SEEK_END) { const auto size=s.Size(); if(size<0) return -1; base=static_cast<std::uint64_t>(size); }
        else if(whence!=SDL_IO_SEEK_SET) { s.Fail("out_of_range"); return -1; }
        if(offset < -static_cast<Sint64>(MaxFilePosition)||offset>static_cast<Sint64>(MaxFilePosition) ||
            (offset<0&&static_cast<std::uint64_t>(-offset)>base) ||
            (offset>=0&&static_cast<std::uint64_t>(offset)>MaxFilePosition-base)) { s.Fail("out_of_range"); return -1; }
        s.position=offset<0?base-static_cast<std::uint64_t>(-offset):base+static_cast<std::uint64_t>(offset);
        return static_cast<Sint64>(s.position);
    }
    static std::size_t SDLCALL Read(void* value,void* data,std::size_t wanted,SDL_IOStatus* status) {
        VFS_PROFILE_SCOPE(NativeRead);
        auto& s=*static_cast<NativeStream*>(value);
        if(!s.mode.readable) { s.Fail("permission_denied"); *status=SDL_IO_STATUS_ERROR; return 0; }
        const auto length=s.Size();
        if(length<0) { *status=SDL_IO_STATUS_ERROR; return 0; }
        if(s.position>=static_cast<std::uint64_t>(length)) { *status=SDL_IO_STATUS_EOF; return 0; }
        const auto size=std::min<std::uint64_t>(wanted,static_cast<std::uint64_t>(length)-s.position);
        if(!s.At(s.position)) { *status=SDL_IO_STATUS_ERROR; return 0; }
        std::size_t done{};
        while(done<size) {
#ifdef _WIN32
            DWORD count{};
            if(!ReadFile(s.file,static_cast<char*>(data)+done,static_cast<DWORD>(std::min<std::uint64_t>(size-done,0x7fffffff)),&count,nullptr)) {
                s.SystemFailure(); *status=SDL_IO_STATUS_ERROR; return done;
            }
#else
            const auto result=::read(s.file,static_cast<char*>(data)+done,std::min<std::uint64_t>(size-done,0x7fffffff));
            if(result<0) { s.SystemFailure(); *status=SDL_IO_STATUS_ERROR; return done; }
            const auto count=static_cast<std::size_t>(result);
#endif
            VFS_PROFILE_ADD(NativeReadBytes,count);
            if(!count) break;
            done+=count; s.position+=count;
        }
        if(done<wanted) *status=SDL_IO_STATUS_EOF;
        return done;
    }
    static std::size_t SDLCALL Write(void* value,const void* data,std::size_t size,SDL_IOStatus* status) {
        auto& s=*static_cast<NativeStream*>(value);
        if(!s.mode.writable) { s.Fail("read_only"); *status=SDL_IO_STATUS_ERROR; return 0; }
        if(!size) return 0;
        const auto length=s.Size();
        if(length<0) { *status=SDL_IO_STATUS_ERROR; return 0; }
        const auto start=s.mode.append?static_cast<std::uint64_t>(length):s.position;
        if(size>MaxFilePosition-start) { s.Fail("out_of_range"); *status=SDL_IO_STATUS_ERROR; return 0; }
        // Explicitly initialize the gap: no dependence on filesystem hole or
        // Win32 file extension contents. A failure may leave partial effects.
        if(!s.mode.append && start>static_cast<std::uint64_t>(length)) {
            if(!s.At(static_cast<std::uint64_t>(length))) { *status=SDL_IO_STATUS_ERROR; return 0; }
            std::array<char,64*1024> zeros{};
            auto gap=start-static_cast<std::uint64_t>(length);
            while(gap) {
                const auto count=s.WriteSome(zeros.data(),static_cast<std::size_t>(std::min<std::uint64_t>(gap,zeros.size())));
                if(!count) { *status=SDL_IO_STATUS_ERROR; return 0; }
                gap-=count;
            }
        }
        if(!s.mode.append && !s.At(start)) { *status=SDL_IO_STATUS_ERROR; return 0; }
        std::size_t done{};
        while(done<size) {
            const auto count=s.WriteSome(static_cast<const char*>(data)+done,size-done);
            if(!count) { if(s.error->code.empty()) s.Fail("io_error"); *status=SDL_IO_STATUS_ERROR; break; }
            done+=count;
            if(s.mode.append) {
                const auto end=s.Size();
                if(end<0) { *status=SDL_IO_STATUS_ERROR; break; }
                s.position=static_cast<std::uint64_t>(end);
            } else s.position=start+done;
        }
        return done;
    }
    static bool SDLCALL Flush(void*,SDL_IOStatus*) { return true; } // No user-space write buffer.
    static bool SDLCALL Close(void* value) {
        std::unique_ptr<NativeStream> s(static_cast<NativeStream*>(value));
#ifdef _WIN32
        const auto file=s->file; s->file=INVALID_HANDLE_VALUE;
        if(!CloseHandle(file)) { s->SystemFailure(); return false; }
#else
        const auto file=s->file; s->file=-1;
        if(::close(file)) { s->SystemFailure(); return false; }
#endif
        return true;
    }
};
#ifdef _WIN32
bool OpenRelative(NativeStream& source,const NativeOpenTarget& target,std::string_view virtualPath) {
    const auto& leaf=target.leaf.native();
    if(!target.parentDirectory||target.parentDirectory==INVALID_HANDLE_VALUE||leaf.empty()||
        leaf==L"."||leaf==L".."||leaf.find_first_of(L"\\/:")!=leaf.npos||leaf.find(L'\0')!=leaf.npos||
        leaf.size()>std::numeric_limits<USHORT>::max()/sizeof(wchar_t))
        throw std::invalid_argument("native relative open requires a verified parent and single filename");
    const auto& mode=source.mode;
    if(mode.exclusive&&target.expectedIdentity) { source.Fail("already_exists"); return false; }
    if(mode.base=='r'&&!target.expectedIdentity) { source.Fail("not_found"); return false; }
    DWORD access=FILE_READ_ATTRIBUTES|SYNCHRONIZE|(mode.readable?GENERIC_READ:0);
    if(mode.writable) access|=mode.append?FILE_APPEND_DATA:GENERIC_WRITE;
    UNICODE_STRING name{};
    name.Buffer=const_cast<PWSTR>(leaf.data());
    name.Length=static_cast<USHORT>(leaf.size()*sizeof(wchar_t)); name.MaximumLength=name.Length;
    OBJECT_ATTRIBUTES object{};
    InitializeObjectAttributes(&object,&name,0,target.parentDirectory,nullptr);
    IO_STATUS_BLOCK io{};
    const auto status=NtCreateFile(&source.file,access,&object,&io,nullptr,FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,target.expectedIdentity?FILE_OPEN:FILE_CREATE,
        FILE_SYNCHRONOUS_IO_NONALERT|FILE_OPEN_REPARSE_POINT|FILE_NON_DIRECTORY_FILE,nullptr,0);
    if(status<0) {
        const auto error=RtlNtStatusToDosError(status);
        source.file=INVALID_HANDLE_VALUE;
        if((target.expectedIdentity&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND))||
            (!target.expectedIdentity&&!mode.exclusive&&(error==ERROR_FILE_EXISTS||error==ERROR_ALREADY_EXISTS)))
            throw NativeViewChanged();
        source.Fail(IoErrorCode({static_cast<int>(error),std::system_category()})); return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    VFS_PROFILE_ADD(AttributeQueries,1);
    if(!GetFileInformationByHandle(source.file,&info)) { source.SystemFailure(); return false; }
    if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||GetFileType(source.file)!=FILE_TYPE_DISK)
        throw AccessError("open '"+std::string(virtualPath)+"': redirected/non-file backend refused");
    FILE_ID_INFO identity{};
    VFS_PROFILE_ADD(IdentityQueries,1);
    if(!GetFileInformationByHandleEx(source.file,FileIdInfo,&identity,sizeof(identity))) { source.SystemFailure(); return false; }
    NativeFileIdentity actual{identity.VolumeSerialNumber,0,0};
    std::memcpy(&actual[1],identity.FileId.Identifier,sizeof(identity.FileId.Identifier));
    if(target.expectedIdentity&&actual!=*target.expectedIdentity) throw NativeViewChanged();
    if(mode.base=='w'&&target.expectedIdentity) {
        // CREATE_ALWAYS refuses existing hidden/system files with our flags.
        // Preserve that result before touching an already verified generation.
        if(info.dwFileAttributes&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM)) { source.Fail("permission_denied"); return false; }
        LARGE_INTEGER zero{};
        if(!SetFilePointerEx(source.file,zero,nullptr,FILE_BEGIN)||!SetEndOfFile(source.file)) {
            source.SystemFailure(); return false;
        }
    }
    return true;
}
#endif
}
SDL_IOStream* OpenNative(const std::filesystem::path& path,const OpenMode& mode,std::string_view virtualPath,
    const NativeOpenTarget* target) {
    VFS_PROFILE_SCOPE(NativeOpen);
    auto source=std::make_unique<NativeStream>(); source->mode=mode;
#ifdef _WIN32
    (void)path;
    if(!target) throw std::invalid_argument("native Windows open requires a verified parent directory");
    if(OpenRelative(*source,*target,virtualPath)) goto opened;
#else
    (void)target;
    int flags=mode.readable?(mode.writable?O_RDWR:O_RDONLY):O_WRONLY;
    if(mode.base=='w') flags|=O_CREAT|O_TRUNC;
    if(mode.append) flags|=O_CREAT|O_APPEND;
    if(mode.exclusive) flags|=O_CREAT|O_EXCL;
    source->file=::open(path.c_str(),flags|O_CLOEXEC|O_NOFOLLOW,0666);
    if(source->file<0) source->SystemFailure(); else goto opened;
#endif
    throw IoError(source->error->code,"open '"+std::string(virtualPath)+"': "+source->error->code);
opened:
    const auto length=source->Size();
    if(length<0) throw IoError(source->error->code,"open '"+std::string(virtualPath)+"': "+source->error->code);
    if(mode.append) source->position=static_cast<std::uint64_t>(length);
    SDL_IOStreamInterface api{}; SDL_INIT_INTERFACE(&api);
    api.size=NativeStream::GetSize; api.seek=NativeStream::Seek;
    api.read=NativeStream::Read; api.write=NativeStream::Write;
    api.flush=NativeStream::Flush; api.close=NativeStream::Close;
    auto* stream=SDL_OpenIO(&api,source.get());
    if(!stream) throw IoError("io_error","open '"+std::string(virtualPath)+"': cannot allocate stream");
    const auto state=source->error;
    auto* native=source.release();
    if(!SDL_SetPointerProperty(SDL_GetIOProperties(stream),NativeStateProperty,native)) {
        SDL_CloseIO(stream); throw IoError("io_error","initialize file metadata: io_error");
    }
    try { SetStreamErrorState(stream,state); } catch(...) { SDL_CloseIO(stream); throw; }
    return stream;
}
std::optional<std::uint64_t> NativeModified(SDL_IOStream* stream,std::string_view path) {
    VFS_PROFILE_SCOPE(NativeModified);
    auto* native=static_cast<NativeStream*>(SDL_GetPointerProperty(SDL_GetIOProperties(stream),NativeStateProperty,nullptr));
    if(!native) return {};
#ifdef _WIN32
    FILETIME modified{};
    VFS_PROFILE_ADD(AttributeQueries,1);
    if(!GetFileTime(native->file,nullptr,nullptr,&modified)) { native->SystemFailure(); throw GetStreamError(stream,"snapshot timestamp",path); }
    return (std::uint64_t(modified.dwHighDateTime)<<32)|modified.dwLowDateTime;
#else
    struct stat info{};
    VFS_PROFILE_ADD(AttributeQueries,1);
    if(fstat(native->file,&info)) { native->SystemFailure(); throw GetStreamError(stream,"snapshot timestamp",path); }
#ifdef __APPLE__
    return std::uint64_t(info.st_mtimespec.tv_sec)*1000000000ULL+static_cast<std::uint64_t>(info.st_mtimespec.tv_nsec);
#else
    return std::uint64_t(info.st_mtim.tv_sec)*1000000000ULL+static_cast<std::uint64_t>(info.st_mtim.tv_nsec);
#endif
#endif
}
}
