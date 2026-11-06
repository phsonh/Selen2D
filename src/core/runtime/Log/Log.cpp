#include "Log.h"

#include <algorithm>
#include <array>
#include <atomic>
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
#ifdef _WIN32
        bool StandardStreamsChecked = false;
        bool StandardStreamsReady = true;
        bool StandardStreamsReported = false;
        DWORD StandardStreamErrors[10]{}; // Restore, control setup, open, repair, verify.
        constexpr const char* StandardStreamOperations[] = {
            "restore stdin", "restore stdout", "restore stderr", "install console control handler",
            "enable Ctrl+C delivery", "open parent console output",
            "repair stdout", "repair stderr", "validate stdout", "validate stderr"};
        HANDLE OutputFallback[2]{}; // Original routes or repaired console after a failed SetStdHandle.
        // At most one handle opened by us, shared by missing output streams.
        // Its lifetime is the process, not Init/Shutdown: late native diagnostics
        // and reInit still use it. The OS reclaims it at process exit. Do not
        // close it per stream or close any borrowed standard handle.
        HANDLE OwnedConsoleOutput = nullptr;
        std::atomic<bool> TerminationPending{false};
        static_assert(std::atomic<bool>::is_always_lock_free);

        BOOL WINAPI ConsoleControl(DWORD event) noexcept
        {
            if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
            TerminationPending.store(true, std::memory_order_relaxed);
            return TRUE;
        }

        bool ValidHandle(HANDLE handle) noexcept
        {
            if (!handle || handle == INVALID_HANDLE_VALUE) return false;
            SetLastError(ERROR_SUCCESS);
            const DWORD type = GetFileType(handle);
            // NUL is a valid CHAR sink even though GetConsoleMode fails.
            // A broken pipe remains a pipe; do not replace deliberate routing
            // just because a later write fails.
            return type != FILE_TYPE_UNKNOWN || GetLastError() == ERROR_SUCCESS;
        }

        HANDLE OutputHandle(bool error) noexcept
        {
            const HANDLE fallback = OutputFallback[error ? 1 : 0];
            return fallback ? fallback : GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
        }

        void PrepareStandardStreams() noexcept
        {
            if (StandardStreamsChecked) return;
            StandardStreamsChecked = true;
            constexpr DWORD ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
            HANDLE original[3];
            bool valid[3];
            for (std::size_t i = 0; i < 3; ++i)
            {
                original[i] = GetStdHandle(ids[i]);
                valid[i] = ValidHandle(original[i]);
            }
            if (valid[1] && valid[2]) return;

            // This is the OS's atomic check-and-attach to the immediate
            // parent's EXISTING console. It cannot allocate a new console.
            // Explorer/services without one simply fail; startup continues.
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

            // Attach resets the handler table. Keep our handler installed until
            // process exit, including native shutdown; removing it earlier
            // would expose cleanup to the default ExitProcess handler again.
            // CTRL_CLOSE is intentionally not handled: Windows can forcibly
            // terminate console clients even if their handler returns TRUE.
            if (!SetConsoleCtrlHandler(ConsoleControl, TRUE))
            {
                StandardStreamsReady = false;
                const DWORD error = GetLastError();
                StandardStreamErrors[3] = error ? error : ERROR_GEN_FAILURE;
            }
            else if (!SetConsoleCtrlHandler(nullptr, FALSE))
            {
                // Unlike the handler table, the inherited ignore-C attribute
                // survives AttachConsole. Enable C for this client only; both
                // C and BREAK must reach the registered request-only handler.
                StandardStreamsReady = false;
                const DWORD error = GetLastError();
                StandardStreamErrors[4] = error ? error : ERROR_GEN_FAILURE;
            }

            // AttachConsole may replace even valid redirected handles. Restore
            // each inherited output independently and preserve stdin exactly.
            // These are borrowed handles: never close them or rebind the CRT.
            for (std::size_t i = 0; i < 3; ++i)
                if ((i == 0 || valid[i]) && GetStdHandle(ids[i]) != original[i] &&
                    !SetStdHandle(ids[i], original[i]))
                {
                    StandardStreamsReady = false;
                    const DWORD error = GetLastError();
                    StandardStreamErrors[i] = error ? error : ERROR_GEN_FAILURE;
                    if (i && valid[i]) OutputFallback[i - 1] = original[i];
                }

            // STARTF_USESTDHANDLES can leave a non-NULL invalid handle intact
            // even after a successful Attach. Only repair originally invalid
            // outputs; a valid inherited file/pipe/NUL must never be replaced.
            bool repair[2]{};
            for (std::size_t i = 0; i < 2; ++i)
                repair[i] = !valid[i + 1] && !ValidHandle(GetStdHandle(ids[i + 1]));
            if (repair[0] || repair[1])
            {
                OwnedConsoleOutput = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (!OwnedConsoleOutput || OwnedConsoleOutput == INVALID_HANDLE_VALUE)
                {
                    StandardStreamsReady = false;
                    const DWORD error = GetLastError();
                    StandardStreamErrors[5] = error ? error : ERROR_GEN_FAILURE;
                    OwnedConsoleOutput = nullptr;
                }
                else
                {
                    for (std::size_t i = 0; i < 2; ++i)
                        if (repair[i] && !SetStdHandle(ids[i + 1], OwnedConsoleOutput))
                        {
                            StandardStreamsReady = false;
                            const DWORD error = GetLastError();
                            StandardStreamErrors[6 + i] = error ? error : ERROR_GEN_FAILURE;
                            OutputFallback[i] = OwnedConsoleOutput;
                        }
                }
            }
            for (std::size_t i = 0; i < 2; ++i)
            {
                SetLastError(ERROR_INVALID_HANDLE);
                if (ValidHandle(GetStdHandle(ids[i + 1]))) continue;
                const DWORD error = GetLastError();
                StandardStreamsReady = false;
                // Keep the causal open/restore/repair error rather than also
                // reporting its expected invalid final slot a second time.
                if (!StandardStreamErrors[i + 1] && !StandardStreamErrors[6 + i] &&
                    !(repair[i] && StandardStreamErrors[5]))
                    StandardStreamErrors[8 + i] = error ? error : ERROR_INVALID_HANDLE;
                if (!OutputFallback[i])
                {
                    const HANDLE fallback = valid[i + 1] ? original[i + 1] : OwnedConsoleOutput;
                    if (ValidHandle(fallback)) OutputFallback[i] = fallback;
                }
            }
            // Writes below use the resulting Win32 standard handles directly:
            // Unicode console output, or unchanged UTF-8 file/pipe bytes.
            // No console mode/code page/window or caller process is changed.
        }
#endif

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
            const HANDLE handle = OutputHandle(error);
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
#ifdef _WIN32
                PrepareStandardStreams();
                if (!StandardStreamsReady)
                {
                    // Do not run a game with a changed stdin/redirection or an
                    // unprotected console attachment. Logging retains any
                    // original valid output whose OS restoration failed.
                    if (!StandardStreamsReported)
                    {
                        StandardStreamsReported = true;
                        for (std::size_t i = 0; i < std::size(StandardStreamErrors); ++i)
                            if (StandardStreamErrors[i])
                            {
                                char message[160];
                                std::snprintf(message, sizeof(message),
                                    "Cannot %s (Windows error %lu); startup aborted", StandardStreamOperations[i],
                                    static_cast<unsigned long>(StandardStreamErrors[i]));
                                WriteLocked("ERROR", message, true, false);
                            }
                    }
                    return false;
                }
#endif
                Initialized = true;
                WriteLocked("INFO ", "Log runtime initialized", false, false);
            }
            return true;
        }
        catch (...) { return false; }
    }
    void Shutdown() noexcept
    {
        // Windows writes above are synchronous. The optional owned CONOUT$
        // handle remains live for reInit/late logs until process exit; all other
        // handles are borrowed. FlushFileBuffers could block on a parent pipe.
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

    bool TerminationRequested() noexcept
    {
#ifdef _WIN32
        return TerminationPending.load(std::memory_order_relaxed);
#else
        return false;
#endif
    }

    bool HasStandardOutputRoute() noexcept
    {
#ifdef _WIN32
        try
        {
            std::lock_guard lock(WriteMutex);
            const auto routed = [](bool error) noexcept {
                SetLastError(ERROR_SUCCESS);
                const HANDLE handle = OutputHandle(error);
                const DWORD queryError = GetLastError();
                if (!handle || handle == INVALID_HANDLE_VALUE)
                    return queryError != ERROR_SUCCESS && queryError != ERROR_INVALID_HANDLE;
                SetLastError(ERROR_SUCCESS);
                const DWORD type = GetFileType(handle);
                // Only a known invalid handle proves no route. Unexpected
                // query failures must not turn automation into a modal dialog.
                return type != FILE_TYPE_UNKNOWN || GetLastError() != ERROR_INVALID_HANDLE;
            };
            return routed(false) || routed(true);
        }
        catch (...) { return true; }
#else
        return true; // No native startup dialog is provided on other platforms.
#endif
    }

    std::string InitializationErrorText() noexcept
    {
#ifdef _WIN32
        try
        {
            std::lock_guard lock(WriteMutex);
            std::string result;
            for (std::size_t i = 0; i < std::size(StandardStreamErrors); ++i)
                if (StandardStreamErrors[i])
                {
                    char message[160];
                    const int length = std::snprintf(message, sizeof(message),
                        "Cannot %s (Windows error %lu); startup aborted", StandardStreamOperations[i],
                        static_cast<unsigned long>(StandardStreamErrors[i]));
                    if (length > 0)
                    {
                        if (!result.empty()) result.push_back('\n');
                        result.append(message, (std::min)(static_cast<std::size_t>(length), sizeof(message) - 1));
                    }
                }
            return result;
        }
        catch (...) {}
#endif
        return {};
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
