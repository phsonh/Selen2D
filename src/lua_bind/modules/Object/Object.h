#pragma once
#include <lua.hpp>
#include <cstdint>
#include <memory>

namespace lua_bind::modules::Object {
    class Context final {
    public:
        Context(lua_State* L, std::uint32_t capacity);
        ~Context();
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        // Host frame boundary only: commit queued destruction/activation.
        // Does not update, render, collide or check world bounds.
        void BeginFrame();

        // Call before lua_close. Invalidates old closures and instances without
        // running game callbacks. During a callback, physical cleanup waits for
        // the outermost binding call to unwind; keep this Context alive until
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
