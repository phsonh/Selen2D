#include "Console.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace core::runtime::Console {
namespace {
std::mutex mutex;
std::string title;
#ifdef _WIN32
HWND window{};
HANDLE output = INVALID_HANDLE_VALUE;
bool owned{};

std::wstring Wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

[[noreturn]] void Error(const char* action, DWORD code) {
    throw std::runtime_error(std::string(action) + " failed (Windows error " + std::to_string(code) + ")");
}

void Create() {
    if (owned) return;
    // GUI-subsystem processes normally have no console, including when launched
    // by a terminal. Do not detach/hide an externally attached console if some
    // embedding host explicitly attached one before calling us.
    DWORD processes[1]{};
    if (GetConsoleProcessList(processes, 1) != 0)
        throw std::runtime_error("Cannot create an engine console while attached to an external console; the external console was left unchanged");

    constexpr DWORD slots[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE previous[3]{};
    for (int i = 0; i < 3; ++i) previous[i] = GetStdHandle(slots[i]);
    if (!AllocConsole()) Error("AllocConsole", GetLastError());
    // AllocConsole may overwrite inherited handles. Keep file/pipe redirection
    // and missing standard handles exactly as the host supplied them. The CRT
    // descriptors are deliberately never freopen/_dup2-rebound.
    for (int i = 0; i < 3; ++i) SetStdHandle(slots[i], previous[i]);
    owned = true;
    window = GetConsoleWindow();
    output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (!window || output == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        output = INVALID_HANDLE_VALUE;
        FreeConsole();
        for (int i = 0; i < 3; ++i) SetStdHandle(slots[i], previous[i]);
        owned = false; window = nullptr;
        Error("Opening engine console", code);
    }
    const auto caption = Wide(title + " | Debug");
    SetConsoleTitleW(caption.c_str());
    // The console X would terminate the process outside Runtime cleanup. The
    // game owns visibility via Debug.SetConsoleVisible(false); do not install
    // a control handler which could affect the caller's terminal.
    if (auto menu = GetSystemMenu(window, FALSE)) DeleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);
}
#endif
}

void SetTitle(std::string_view value) {
    std::lock_guard lock(mutex);
    title.assign(value);
}

void SetVisible(bool visible) {
    std::lock_guard lock(mutex);
#ifdef _WIN32
    if (visible) Create();
    if (owned && window) ShowWindow(window, visible ? SW_SHOW : SW_HIDE);
#else
    if (visible) throw std::runtime_error("The engine debug console is supported only on Windows");
#endif
}

bool IsVisible() noexcept {
    std::lock_guard lock(mutex);
#ifdef _WIN32
    return owned && window && IsWindow(window) && IsWindowVisible(window);
#else
    return false;
#endif
}

void Write(std::string_view text) noexcept {
    try {
        std::lock_guard lock(mutex);
#ifdef _WIN32
        // This sink is independent of stdout/stderr and remains allocated while
        // hidden. Output redirection and the persistent file sink keep working.
        if (!owned || output == INVALID_HANDLE_VALUE) return;
        const auto wide = Wide(text);
        std::size_t offset{};
        while (offset < wide.size()) {
            DWORD count = static_cast<DWORD>((std::min)(wide.size() - offset, std::size_t(16 * 1024)));
            if (offset + count < wide.size() && wide[offset + count - 1] >= 0xd800 && wide[offset + count - 1] <= 0xdbff) --count;
            DWORD written{};
            if (!WriteConsoleW(output, wide.data() + offset, count, &written, nullptr) || !written) break;
            offset += written;
        }
#else
        (void)text;
#endif
    } catch (...) {
        // A diagnostic mirror must never replace a Lua error or interrupt cleanup.
    }
}

void Shutdown() noexcept {
    std::lock_guard lock(mutex);
#ifdef _WIN32
    if (!owned) return;
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    output = INVALID_HANDLE_VALUE;
    // Only release the console this service created, never a caller's console.
    FreeConsole();
    owned = false; window = nullptr;
#endif
}
}
