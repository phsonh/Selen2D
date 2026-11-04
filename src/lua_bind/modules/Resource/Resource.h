#pragma once
#include <lua.hpp>
#include <memory>
#include <functional>

namespace core::modules::Resource { class Service; class Job; struct Asset; }
namespace core::modules::File { class FileSystem; }
namespace lua_bind::modules::Resource {
    // Internal binding boundary: ready logical Image name only.
    // Renderer never receives a resource registry or Lua value.
    std::shared_ptr<const core::modules::Resource::Asset> AcquireImage(
        lua_State*, int index, core::modules::Resource::Service&);
    // Borrows the native service. The host must invalidate this context before
    // destroying Service; neither Lua GC nor this context owns its lifetime.
    class Context final {
    public:
        Context(lua_State* L, core::modules::Resource::Service& service,
            std::function<void(core::modules::Resource::Job&)> wait,
            std::shared_ptr<const core::modules::File::FileSystem> files);
        ~Context();
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        // No Lua calls, no resource cancellation, safe to call repeatedly.
        void Shutdown() noexcept;
    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        friend void Register(lua_State*, Context&);
    };

    // Preserves the stack. Coroutines share the same VM registration. A second
    // live context cannot replace the first; old closures never reconnect.
    void Register(lua_State* L, Context& context);
}
