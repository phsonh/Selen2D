#include "DxcRuntime.h"
#include "core/runtime/Log/Log.h"
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <dxc/dxcapi.h>

namespace core::modules::Graphic::Shader::DxcRuntime {
namespace {
std::string Utf8(const std::wstring& value) {
    if(value.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("DXC runtime path is too long to encode");
    const auto size=static_cast<int>(value.size());
    const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),size,nullptr,0,nullptr,nullptr);
    if(!count) throw std::runtime_error("DXC runtime path is not valid UTF-16");
    std::string result(static_cast<std::size_t>(count),'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),size,result.data(),count,nullptr,nullptr))
        throw std::runtime_error("DXC runtime path UTF-8 conversion failed");
    return result;
}
[[noreturn]] void Fail(const char* operation,const std::wstring& path,DWORD error) {
    if(!error) error=ERROR_GEN_FAILURE;
    throw std::runtime_error(std::string("DXC runtime ")+operation+" failed for \""+Utf8(path)+
        "\" (Win32 error "+std::to_string(error)+")");
}
std::wstring ModulePath(HMODULE module,const std::wstring& context) {
    std::vector<wchar_t> buffer(256);
    for(;;) {
        const DWORD length=GetModuleFileNameW(module,buffer.data(),static_cast<DWORD>(buffer.size()));
        if(!length) Fail("GetModuleFileNameW",context,GetLastError());
        if(length<buffer.size()) return std::wstring(buffer.data(),length);
        if(buffer.size()>std::numeric_limits<DWORD>::max()/2)
            Fail("GetModuleFileNameW",context,ERROR_INSUFFICIENT_BUFFER);
        buffer.resize(buffer.size()*2);
    }
}
struct State {
    std::once_flag once;
    // Deliberately not an unloading RAII handle: compiler workers, COM owners
    // and late process teardown must all keep the same implementation alive.
    HMODULE module{};
    DxcCreateInstanceProc create{};
    std::exception_ptr failure;
};
State& Runtime() {
    static State state;
    std::call_once(state.once,[&] {
        try {
            const std::filesystem::path executable(ModulePath(nullptr,L"current executable"));
            if(!executable.is_absolute() || !executable.has_filename())
                Fail("executable path validation",executable.native(),ERROR_BAD_PATHNAME);
            const auto path=(executable.parent_path()/L"dxcompiler.dll").native();
            state.module=LoadLibraryExW(path.c_str(),nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!state.module) Fail("LoadLibraryExW",path,GetLastError());
            const auto address=GetProcAddress(state.module,"DxcCreateInstance");
            if(!address) Fail("GetProcAddress(DxcCreateInstance)",path,GetLastError());
            state.create=reinterpret_cast<DxcCreateInstanceProc>(address);
            const auto loaded=ModulePath(state.module,path);
            core::runtime::Log::Debug("DXC runtime loaded: "+Utf8(loaded));
        } catch(...) {
            // Returning normally marks call_once complete. Throwing out of it
            // would retry DLL discovery on subsequent resource worker calls.
            state.failure=std::current_exception();
        }
    });
    if(state.failure) std::rethrow_exception(state.failure);
    return state;
}
}
void Initialize() { (void)Runtime(); }
std::int32_t Create(const _GUID& clsid,const _GUID& iid,void** object) {
    return static_cast<std::int32_t>(Runtime().create(clsid,iid,object));
}
}
