#pragma once

#include <string>
#include <string_view>

namespace core::runtime::Log
{
    // No files or consoles are created. Reinitializing preserves Lua history.
    bool Init() noexcept;
    void Shutdown() noexcept;

    void Debug(std::string_view message) noexcept;
    void Info(std::string_view message) noexcept;
    void Warn(std::string_view message) noexcept;
    void Error(std::string_view message) noexcept;
    void Fatal(std::string_view message) noexcept;

    // Only these entry points retain records for Debug.GetLog(). Native logs
    // use the functions above. Embedded NUL bytes are preserved by length.
    void LuaInfo(std::string_view message) noexcept;
    void LuaWarn(std::string_view message) noexcept;
    void LuaError(std::string_view message) noexcept;

    // An owned, chronological snapshot; no logger lock escapes this call.
    // History has a 1 MiB / 8192-record bound and evicts whole old records.
    // Once content is lost, snapshots retain a history-truncated marker.
    // Oversized records retain their UTF-8-complete tail with a local marker.
    std::string GetLuaText();
}
