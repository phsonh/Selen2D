#pragma once
#include <lua.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace lua_bind::modules::Object {
    class Context final {
    public:
        Context(lua_State* L, std::uint32_t capacity);
        ~Context();
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        // Call before ExitFunction: keeps the service alive, but public Commit
        // can no longer activate anything. A mere Runtime.Quit request is not
        // this boundary; the current Lua call may finish normally first.
        void BeginShutdown() noexcept;

        // Call after ExitFunction, before destroying its dependent services or
        // the Lua VM. Drain requested Delete callbacks (including tail additions)
        // once, keep going after cleanup errors, then release all other objects
        // without callbacks or activation. False reports cleanup failure; error
        // contains its first diagnostic. Repeated calls preserve that result.
        bool FinalizeShutdown(std::string& error);

        // Emergency/destructor fallback, before lua_close. Invalidates old
        // closures and instances without game callbacks. Normal host shutdown
        // must use BeginShutdown/FinalizeShutdown first. During a callback,
        // physical cleanup waits for the outermost binding call to unwind;
        // keep this Context alive until
        // that call returns. Calling Shutdown more than once is harmless.
        void Shutdown() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        friend void Register(lua_State* L, Context& context);
    };

    // The same live Context may be registered again. Another live Context
    // cannot register in this VM. Preserves the caller's Lua stack.
    void Register(lua_State* L, Context& context);
}
