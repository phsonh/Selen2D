#include "engine/Engine.h"
#include "engine/Startup.h"
#include "core/modules/File/Profile.h"
#include "core/runtime/Log/Log.h"
#include <SDL3/SDL.h>
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
std::vector<std::string> Arguments(int argc,char* argv[]) {
    std::vector<std::string> result;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    // The CRT narrow argv uses the system code page. Take the native Unicode
    // argv once and convert each argument without losing empty strings/boundaries.
    int count{};
    std::unique_ptr<wchar_t*,decltype(&LocalFree)> values(
        CommandLineToArgvW(GetCommandLineW(),&count),&LocalFree);
    if(!values) throw std::runtime_error("Cannot obtain Unicode startup arguments");
    for(int i=1;i<count;++i) {
        const std::wstring_view value(values.get()[i]);
        if(value.empty()) { result.emplace_back(); continue; }
        const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),
            static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
        if(!bytes) throw std::runtime_error("Startup argument is not valid Unicode");
        std::string utf8(static_cast<std::size_t>(bytes),'\0');
        if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),
            static_cast<int>(value.size()),utf8.data(),bytes,nullptr,nullptr))
            throw std::runtime_error("Cannot convert startup argument to UTF-8");
        result.push_back(std::move(utf8));
    }
#else
    for(int i=1;i<argc;++i) result.emplace_back(argv[i]);
#endif
    return result;
}
}

int RunApplication(int argc, char* argv[]) {
    core::modules::File::profile::Initialize();
    struct ProfileLifetime { ~ProfileLifetime() { core::modules::File::profile::Flush(); } } profile;
    // engine_app is a GUI-subsystem executable on Windows: the OS never opens
    // an initial console to hide later. Diagnostics stay available until all
    // engine cleanup has finished; they never open a persistent log file.
    struct DiagnosticsLifetime {
        ~DiagnosticsLifetime() {
            core::runtime::Log::Shutdown();
        }
    } diagnostics;
    core::runtime::Log::Init();
    engine::Startup::Config config;
    std::uint64_t frame_limit{};
    std::vector<std::string> arguments;
    try {
        arguments=Arguments(argc,argv);
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const std::string_view arg(arguments[i]);
            if(arg=="--") break; // Still retained verbatim by GetArguments.
            if (arg != "--frames") continue; // Application options belong to Lua.
            if(i+1==arguments.size()) throw std::invalid_argument("--frames requires a positive integer");
            const std::string_view value(arguments[++i]);
            const auto result = std::from_chars(value.data(), value.data() + value.size(), frame_limit);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || frame_limit == 0) {
                throw std::invalid_argument("--frames requires a positive integer");
            }
        }
        // The deployment root is used only by startup/VFS, not by diagnostics.
        const auto* directory = SDL_GetBasePath();
        if(!directory) throw std::runtime_error(std::string("Cannot locate executable directory: ")+SDL_GetError());
        const auto deployment = std::filesystem::u8path(directory);
        config=engine::Startup::Load(deployment);
    } catch(const std::exception& error) {
        core::runtime::Log::Error(std::string("Startup error: ") + error.what());
        return 2;
    }
    const bool initialized = engine::Init(config.entry,config.files,arguments);
    const bool ran = initialized && engine::Run(frame_limit);
    const bool closed = engine::Shutdown();
    return initialized && ran && closed ? 0 : 1;
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return RunApplication(0, nullptr); // Arguments() reads the Unicode command line.
}
#else
int main(int argc, char* argv[]) {
    return RunApplication(argc, argv);
}
#endif
