#pragma once
#include <string>
#include <string_view>
#include <thread>

namespace engine::StartupFailure {
enum class Kind { Logging, Configuration, Dxc, NativeServices };

// Internal result only; Lua entry failures and later runtime errors do not set it.
struct Failure {
    Kind kind{Kind::NativeServices};
    std::string detail;
    bool available{};
    void Set(Kind value, std::string_view message) noexcept {
        kind=value;
        available=true;
        try { detail.assign(message); } catch(...) { detail.clear(); }
    }
};

// Owned by the application entry thread. It never logs, creates a console, or
// participates in engine cleanup. Call only after startup resources are closed.
class Presentation {
public:
    Presentation() noexcept;
    void Show(Kind kind, std::string_view detail) noexcept;
private:
    std::thread::id thread_;
    bool suppressed_{true};
    bool attempted_{};
};
}
