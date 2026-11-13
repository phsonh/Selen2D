#include "StartupFailure.h"
#include "BuildInfo.h"
#include <limits>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <wtsapi32.h>
#endif

namespace engine::StartupFailure {
const char* Phase(Kind kind) noexcept {
    switch(kind) {
    case Kind::Logging: return "Diagnostic initialization";
    case Kind::Configuration: return "Startup configuration / entry loading";
    case Kind::Dxc: return "HLSL compiler initialization";
    case Kind::NativeServices: return "Native service initialization";
    case Kind::LuaEntry: return "Lua entry";
    case Kind::LuaFrame: return "Lua frame callback";
    case Kind::LuaExit: return "Lua exit callback";
    case Kind::ObjectCleanup: return "Object shutdown callbacks";
    case Kind::Runtime: return "Host runtime";
    default: return "Native cleanup";
    }
}
namespace {
#ifdef _WIN32
bool MayShowOnCurrentDesktop() noexcept {
    // Output routes, an attached console and a debugger do not suppress an
    // interactive fatal-error dialog. Never inspect/rebind those routes here.
    DWORD session{};
    if(!ProcessIdToSessionId(GetCurrentProcessId(),&session) || !session) return false;
    LPWSTR information{};
    DWORD informationBytes{};
    if(!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSConnectState,
        &information,&informationBytes)) return false;
    const bool active=information && informationBytes>=sizeof(WTS_CONNECTSTATE_CLASS) &&
        *reinterpret_cast<const WTS_CONNECTSTATE_CLASS*>(information)==WTSActive;
    WTSFreeMemory(information);
    if(!active) return false;

    const auto station=GetProcessWindowStation(); // Borrowed, never close it.
    USEROBJECTFLAGS flags{};
    DWORD required{};
    if(!station || !GetUserObjectInformationW(station,UOI_FLAGS,&flags,sizeof(flags),&required) ||
        !(flags.dwFlags&WSF_VISIBLE)) return false;
    const auto desktop=GetThreadDesktop(GetCurrentThreadId()); // Borrowed.
    BOOL receivingInput{};
    return desktop && GetUserObjectInformationW(desktop,UOI_IO,&receivingInput,
        sizeof(receivingInput),&required) && receivingInput;
}

std::wstring Wide(std::string_view text) {
    if(text.empty()) return {};
    if(text.size()>static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    const auto size=static_cast<int>(text.size());
    const int length=MultiByteToWideChar(CP_UTF8,0,text.data(),size,nullptr,0);
    if(!length) return {};
    std::wstring result(static_cast<std::size_t>(length),L'\0');
    if(!MultiByteToWideChar(CP_UTF8,0,text.data(),size,result.data(),length)) return {};
    for(auto& value:result) if(!value) value=L'\x2400';
    return result;
}

const wchar_t* Explanation(Kind kind) noexcept {
    switch(kind) {
    case Kind::Logging:
        return L"The engine could not prepare its diagnostic output.\n"
            L"Try launching from a terminal with valid stdout and stderr routes.";
    case Kind::Configuration:
        return L"The engine could not validate its startup configuration or entry files.\n"
            L"Check the entry script, authorized directories, and required resource packages.";
    case Kind::Dxc:
        return L"The engine could not initialize the HLSL compiler.\n"
            L"Extract the complete application directory and keep its matching dxcompiler.dll beside the EXE. "
            L"A missing, unreadable, damaged, incompatible, or incomplete DLL can cause this failure.";
    case Kind::LuaEntry: case Kind::LuaFrame: case Kind::LuaExit: case Kind::ObjectCleanup:
        return L"An unhandled Lua error stopped this run.\n"
            L"Check the script location and traceback below.";
    case Kind::Runtime: case Kind::Cleanup:
        return L"An unrecoverable host error stopped this run.\n"
            L"Check the application resources and the diagnostic details below.";
    default:
        return L"The engine could not initialize a required native service.\n"
            L"Check the application files and available graphics, audio, and system services.";
    }
}
#endif
}

Presentation::Presentation() noexcept : thread_(std::this_thread::get_id()) {
#ifdef _WIN32
    // This scan precedes Log::Init and the ordinary argument parser. A malformed
    // --frames option must not defeat an automation caller's explicit opt-out.
    int count{};
    auto* arguments=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!arguments) return; // Uncertain arguments: never risk a blocking dialog.
    bool suppress=count<1;
    for(int i=1;i<count;++i) {
        const std::wstring_view argument(arguments[i]);
        if(argument==L"--") break;
        if(argument==L"--no-error-dialog") suppress=true;
    }
    LocalFree(arguments);
    suppressed_=suppress;
#endif
}

void Presentation::Show(Kind kind,std::string_view detail) noexcept {
    if(attempted_ || suppressed_ || std::this_thread::get_id()!=thread_) return;
    attempted_=true;
#ifdef _WIN32
    if(!MayShowOnCurrentDesktop()) return;
    try {
        auto title=Wide(build::Product);
        title.append(L" - Error");
        std::wstring message(Explanation(kind));
        message.append(L"\n\nPhase: "); message.append(Wide(Phase(kind)));
        if(!detail.empty()) {
            message.append(L"\n\nTechnical details:\n");
            message.append(Wide(detail));
        }
        MessageBoxW(nullptr,message.c_str(),title.c_str(),MB_OK|MB_ICONERROR|MB_SETFOREGROUND);
    } catch(...) {
        // Presentation is best-effort and never replaces the logged failure or
        // its original exit code, including allocation/encoding failure here.
    }
#else
    (void)kind;
    (void)detail;
#endif
}
void Presentation::Show(const Failure& failure) noexcept {
    if(!failure.available) return;
    try {
        auto message=failure.detail;
        if(!failure.additional.empty()) { message+="\n\nAdditional failures:"; message+=failure.additional; }
        if(failure.truncated) message+="\n[Additional diagnostics omitted; see stderr.]";
        Show(failure.kind,message);
    } catch(...) { Show(failure.kind,failure.detail); }
}
}
