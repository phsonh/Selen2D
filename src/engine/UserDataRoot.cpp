#include "UserDataRoot.h"
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ShlObj.h>
#include <memory>
#else
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>
#include <vector>
#endif

namespace engine::Startup {
std::filesystem::path ResolveUserDataRoot() {
    std::filesystem::path root;
#ifdef _WIN32
    PWSTR raw{};
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &raw);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
    if(FAILED(result) || !raw) throw std::runtime_error("Cannot resolve system LocalAppData");
    root = raw;
#else
    if(const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg && std::filesystem::path(xdg).is_absolute()) root = xdg;
    else {
        std::filesystem::path home;
        if(const char* value = std::getenv("HOME"); value && *value && std::filesystem::path(value).is_absolute()) home = value;
        else {
            long suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
            std::vector<char> buffer(static_cast<std::size_t>(suggested > 0 ? suggested : 16384));
            passwd entry{}, *resolved{};
            if(getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &resolved) != 0 || !resolved || !entry.pw_dir)
                throw std::runtime_error("Cannot resolve system user home");
            home = entry.pw_dir;
        }
        if(!home.is_absolute()) throw std::runtime_error("System user home is not absolute");
        root = home / ".local" / "share";
    }
#endif
    if(root.empty() || !root.is_absolute()) throw std::runtime_error("System user data root is not absolute");
    return root.lexically_normal();
}
}
