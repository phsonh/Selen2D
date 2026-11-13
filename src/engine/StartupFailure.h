#pragma once
#include <string>
#include <string_view>
#include <thread>

namespace engine::StartupFailure {
enum class Kind { Logging, Configuration, Dxc, NativeServices, LuaEntry, LuaFrame, LuaExit, ObjectCleanup, Runtime, Cleanup };
const char* Phase(Kind kind) noexcept;

// Explicit host failure record. Ordinary log messages never set this state.
struct Failure {
    Kind kind{Kind::NativeServices};
    std::string detail;
    std::string additional;
    bool truncated{};
    bool available{};
    void Set(Kind value, std::string_view message) noexcept {
        if(!available) {
            kind=value; available=true;
            try { detail.assign(message); } catch(...) { detail.clear(); }
            return;
        }
        // Keep the first failure intact. Supplemental presentation is bounded;
        // each diagnostic is still sent independently to stderr by its owner.
        constexpr std::size_t limit=64*1024;
        if(truncated) return;
        try {
            std::string next="\n\n"; next+=Phase(value); next+=":\n"; next+=message;
            if(next.size()>limit-additional.size()) { truncated=true; return; }
            additional+=next;
        } catch(...) { truncated=true; }
    }
};

// Owned by the application entry thread. It never logs, creates a console, or
// participates in engine cleanup. Call only after safe cleanup has completed.
class Presentation {
public:
    Presentation() noexcept;
    void Show(Kind kind, std::string_view detail) noexcept;
    void Show(const Failure& failure) noexcept;
private:
    std::thread::id thread_;
    bool suppressed_{true};
    bool attempted_{};
};
}
