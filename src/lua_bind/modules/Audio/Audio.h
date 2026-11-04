#pragma once
#include <lua.hpp>
#include <memory>
namespace core::modules::Audio { class AudioSystem; }
namespace core::modules::Resource { class Service; }
namespace lua_bind::modules::Audio {
class Context final {
public:
    Context(lua_State*, core::modules::Audio::AudioSystem&, core::modules::Resource::Service&);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    // Call while the Lua state is alive, before lua_close. Releases the current
    // userdata root and revokes all binding tokens; native drain is host-owned.
    void Shutdown() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend void Register(lua_State*, Context&);
};
void Register(lua_State*, Context&);
}
