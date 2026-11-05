#include "Log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace core::runtime::Log
{
    namespace
    {
        constexpr std::size_t ByteCapacity = 1024 * 1024;
        constexpr std::size_t RecordCapacity = 8192;
        constexpr std::string_view HistoryMarker = "[Lua log history truncated]\n";
        constexpr std::string_view Truncated = "...[truncated] ";
        constexpr std::size_t StorageCapacity = ByteCapacity - HistoryMarker.size();

        struct Record
        {
            std::uint32_t start = 0;
            std::uint32_t length = 0;
        };

        // Fixed storage lets the error path retain a Lua diagnostic without
        // allocating. Snapshot allocation is deliberately in GetLuaText only.
        std::array<char, StorageCapacity> Bytes{};
        std::array<Record, RecordCapacity> Records{};
        std::size_t FirstRecord = 0;
        std::size_t RecordCount = 0;
        std::size_t NextByte = 0;
        std::size_t UsedBytes = 0;
        bool HistoryTruncated = false;
        bool Initialized = false;
        std::mutex WriteMutex;

        std::size_t Utf8Prefix(std::string_view text, std::size_t length) noexcept
        {
            if (length >= text.size()) return text.size();
            // If the first omitted byte continues a character, omit that
            // character in full. Invalid input is otherwise retained verbatim.
            while (length && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80)
                --length;
            return length;
        }

        void AppendBytes(std::string_view text) noexcept
        {
            if (text.empty()) return;
            const std::size_t first = (std::min)(text.size(), StorageCapacity - NextByte);
            std::memcpy(Bytes.data() + NextByte, text.data(), first);
            if (first < text.size())
                std::memcpy(Bytes.data(), text.data() + first, text.size() - first);
            NextByte = (NextByte + text.size()) % StorageCapacity;
            UsedBytes += text.size();
        }

        void Retain(std::string_view prefix, std::string_view message) noexcept
        {
            const bool truncated = message.size() > StorageCapacity - prefix.size() - 1;
            if (truncated)
            {
                std::size_t start = message.size() - (StorageCapacity - prefix.size() - Truncated.size() - 1);
                // Retain the end of oversized diagnostics, without starting
                // in the middle of a UTF-8 character.
                while (start < message.size() && (static_cast<unsigned char>(message[start]) & 0xc0) == 0x80)
                    ++start;
                message.remove_prefix(start);
                HistoryTruncated = true;
            }
            const std::size_t length = prefix.size() + message.size() + 1 + (truncated ? Truncated.size() : 0);
            while (RecordCount && (RecordCount == RecordCapacity || UsedBytes > StorageCapacity - length))
            {
                HistoryTruncated = true;
                UsedBytes -= Records[FirstRecord].length;
                FirstRecord = (FirstRecord + 1) % RecordCapacity;
                --RecordCount;
            }
            Records[(FirstRecord + RecordCount) % RecordCapacity] =
                {static_cast<std::uint32_t>(NextByte), static_cast<std::uint32_t>(length)};
            ++RecordCount;
            AppendBytes(prefix);
            if (truncated) AppendBytes(Truncated);
            AppendBytes(message);
            AppendBytes("\n");
        }

        std::size_t MakePrefix(char* buffer, std::size_t capacity, const char* level) noexcept
        {
            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &time);
#else
            localtime_r(&time, &local);
#endif
            const auto fraction = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count() % 1000;
            const int length = std::snprintf(buffer, capacity,
                "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] ",
                local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                local.tm_hour, local.tm_min, local.tm_sec,
                static_cast<int>(fraction < 0 ? fraction + 1000 : fraction), level);
            return length > 0 ? (std::min)(static_cast<std::size_t>(length), capacity - 1) : 0;
        }

#ifdef _WIN32
        template<class Writer>
        void WriteWide(std::string_view text, Writer&& writer) noexcept
        {
            constexpr std::size_t ChunkSize = 4096;
            wchar_t converted[ChunkSize + 1];
            while (!text.empty())
            {
                std::size_t count = Utf8Prefix(text, (std::min)(text.size(), ChunkSize));
                if (!count) count = (std::min)(text.size(), ChunkSize);
                // Flags=0 replaces malformed UTF-8 for text-only sinks. Pipe,
                // file and cached records keep the original bytes unchanged.
                const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                    static_cast<int>(count), converted, static_cast<int>(ChunkSize));
                if (length > 0 && !writer(converted, static_cast<DWORD>(length))) return;
                text.remove_prefix(count);
            }
        }

        void WriteHandle(HANDLE handle, bool console, std::string_view text) noexcept
        {
            if (!handle || handle == INVALID_HANDLE_VALUE) return;
            if (console)
            {
                WriteWide(text, [handle](wchar_t* data, DWORD length) noexcept
                {
                    while (length)
                    {
                        DWORD written = 0;
                        if (!WriteConsoleW(handle, data, length, &written, nullptr) || !written) return false;
                        data += written;
                        length -= written;
                    }
                    return true;
                });
                return;
            }
            while (!text.empty())
            {
                // Bounded writes work with inherited pipes as well as files;
                // do not assume that one successful call writes everything.
                const DWORD count = static_cast<DWORD>((std::min)(text.size(), std::size_t(64 * 1024)));
                DWORD written = 0;
                if (!WriteFile(handle, text.data(), count, &written, nullptr) || !written) return;
                text.remove_prefix(written);
            }
        }

        void WriteDebugger(std::string_view text) noexcept
        {
            WriteWide(text, [](wchar_t* data, DWORD length) noexcept
            {
                // OutputDebugString is NUL-terminated. Show embedded NULs as
                // U+2400 there only, so the rest of a diagnostic is visible.
                for (DWORD i = 0; i < length; ++i)
                    if (!data[i]) data[i] = L'\x2400';
                data[length] = L'\0';
                OutputDebugStringW(data);
                return true;
            });
        }
#else
        void WriteFileStream(std::FILE* stream, std::string_view text) noexcept
        {
            while (!text.empty())
            {
                const auto written = std::fwrite(text.data(), 1, text.size(), stream);
                if (!written) return;
                text.remove_prefix(written);
            }
        }
#endif

        void WriteLocked(const char* level, std::string_view message, bool error, bool lua) noexcept
        {
            char prefixBuffer[64];
            const std::string_view prefix(prefixBuffer, MakePrefix(prefixBuffer, sizeof(prefixBuffer), level));
            if (lua) Retain(prefix, message);
#ifdef _WIN32
            // Query inherited handles at each write, including GUI builds
            // whose CRT streams are not connected to the parent handles.
            const HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
            DWORD mode = 0;
            const bool console = handle && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode);
            WriteHandle(handle, console, prefix);
            WriteHandle(handle, console, message);
            WriteHandle(handle, console, "\n");
            WriteDebugger(prefix);
            WriteDebugger(message);
            WriteDebugger("\n");
#else
            std::FILE* stream = error ? stderr : stdout;
            WriteFileStream(stream, prefix);
            WriteFileStream(stream, message);
            WriteFileStream(stream, "\n");
            std::fflush(stream);
#endif
        }

        void Write(const char* level, std::string_view message, bool error, bool lua) noexcept
        {
            try
            {
                std::lock_guard lock(WriteMutex);
                WriteLocked(level, message, error, lua);
            }
            catch (...)
            {
                // Logging must never replace the original error or recurse.
            }
        }
    }

    bool Init() noexcept
    {
        try
        {
            std::lock_guard lock(WriteMutex);
            if (!Initialized)
            {
                Initialized = true;
                WriteLocked("INFO ", "Log runtime initialized", false, false);
            }
            return true;
        }
        catch (...) { return false; }
    }
    void Shutdown() noexcept
    {
        // Windows writes above are synchronous and own no handles. In
        // particular, FlushFileBuffers could block on an inherited pipe.
        try
        {
            std::lock_guard lock(WriteMutex);
            if (!Initialized) return;
            WriteLocked("INFO ", "Log runtime shutdown", false, false);
            Initialized = false;
#ifndef _WIN32
            std::fflush(stdout);
            std::fflush(stderr);
#endif
        }
        catch (...) {}
    }

    void Debug(std::string_view message) noexcept { Write("DEBUG", message, false, false); }
    void Info(std::string_view message) noexcept { Write("INFO ", message, false, false); }
    void Warn(std::string_view message) noexcept { Write("WARN ", message, true, false); }
    void Error(std::string_view message) noexcept { Write("ERROR", message, true, false); }
    void Fatal(std::string_view message) noexcept { Write("FATAL", message, true, false); }
    void LuaInfo(std::string_view message) noexcept { Write("INFO ", message, false, true); }
    void LuaWarn(std::string_view message) noexcept { Write("WARN ", message, true, true); }
    void LuaError(std::string_view message) noexcept { Write("ERROR", message, true, true); }

    std::string GetLuaText()
    {
        std::lock_guard lock(WriteMutex);
        std::string result;
        result.reserve(UsedBytes + (HistoryTruncated ? HistoryMarker.size() : 0));
        if (HistoryTruncated) result.append(HistoryMarker);
        for (std::size_t i = 0; i < RecordCount; ++i)
        {
            const Record& record = Records[(FirstRecord + i) % RecordCapacity];
            const std::size_t first = (std::min)(std::size_t(record.length), StorageCapacity - record.start);
            result.append(Bytes.data() + record.start, first);
            if (first < record.length) result.append(Bytes.data(), record.length - first);
        }
        return result;
    }
}
