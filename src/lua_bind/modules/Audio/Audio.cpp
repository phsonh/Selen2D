#include "Audio.h"
#include "core/modules/Audio/Audio.h"
#include "core/modules/Resource/AudioAsset.h"
#include "lua_bind/Guard.h"
#include <cmath>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>

namespace lua_bind::modules::Audio {
namespace {
namespace Native = core::modules::Audio;
namespace Resource = core::modules::Resource;
constexpr const char* ServiceType = "Solongo.Audio.Service";
constexpr std::uint64_t Tag = 0x53415544494f5356ULL;
char ContextKey;
struct Token {
    Native::AudioSystem* audio{};
    Resource::Service* resources{};
    std::shared_ptr<Native::Playback> currentMusic;
};
struct ServiceBox { std::shared_ptr<Token> token; };
struct Slot { std::uint64_t tag{Tag}; ServiceBox* box{}; };
Slot* TrySlot(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TUSERDATA || lua_objlen(L, index) != sizeof(Slot) || !lua_getmetatable(L, index)) return nullptr;
    luaL_getmetatable(L, ServiceType);
    const bool matches = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    if (!matches) return nullptr;
    auto* slot = static_cast<Slot*>(lua_touserdata(L, index));
    return slot->tag == Tag ? slot : nullptr;
}
int Collect(lua_State* L) {
    if (auto* slot = TrySlot(L, 1)) { delete slot->box; slot->box = nullptr; }
    return 0;
}
Token& Host(lua_State* L) {
    auto* slot = TrySlot(L, lua_upvalueindex(1));
    if (!slot || !slot->box || !slot->box->token || !slot->box->token->audio || !slot->box->token->resources)
        throw std::runtime_error("Audio Context has shut down");
    return *slot->box->token;
}
void Count(lua_State* L, int expected) {
    if (lua_gettop(L) != expected) throw std::invalid_argument("Audio: invalid argument count");
    if (!lua_checkstack(L, 5)) throw std::runtime_error("Audio Lua stack capacity exceeded");
}
std::shared_ptr<Native::Source> Source(lua_State* L, Token& host, Resource::Kind kind) {
    const auto text = CheckString(L, 1);
    if (text.empty() || text.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Audio resource name must be nonempty and contain no NUL");
    const std::string name(text);
    const auto handle = host.resources->Find(name);
    if (!handle) throw std::runtime_error("audio resource does not exist: " + name);
    if (host.resources->GetInfo(*handle).id.kind != kind)
        throw std::invalid_argument("audio resource has the wrong type: " + name);
    const auto asset = std::dynamic_pointer_cast<const Resource::AudioAsset>(host.resources->Acquire(*handle));
    if (!asset || !asset->source) throw std::runtime_error("audio resource has no prepared source: " + name);
    return asset->source;
}
double Volume(lua_State* L) {
    if (lua_type(L, 1) != LUA_TNUMBER) throw std::invalid_argument("Audio volume must be a number");
    const auto value = lua_tonumber(L, 1);
    // Preserve Lua's double until the final SDL gain calculation.
    if (!std::isfinite(value) || value < 0 || value > 1)
        throw std::invalid_argument("Audio volume must be finite and within [0,1]");
    return value;
}
int PlaySound(lua_State* L) {
    Count(L, 1); auto& host = Host(L);
    auto source = Source(L, host, Resource::Kind::Sound);
    Native::PlayOptions options;
    options.policy = Native::PlayPolicy::Overlap;
    options.bus = "sound";
    host.audio->Play(source, options);
    return 0;
}
int StopSound(lua_State* L) {
    Count(L, 1); auto& host = Host(L);
    host.audio->StopSource(Source(L, host, Resource::Kind::Sound)); return 0;
}
int PauseSound(lua_State* L) {
    Count(L, 1); auto& host = Host(L);
    host.audio->PauseSource(Source(L, host, Resource::Kind::Sound)); return 0;
}
int ResumeSound(lua_State* L) {
    Count(L, 1); auto& host = Host(L);
    host.audio->ResumeSource(Source(L, host, Resource::Kind::Sound)); return 0;
}
int PlayMusic(lua_State* L) {
    Count(L, 1); auto& host = Host(L);
    auto source = Source(L, host, Resource::Kind::Music); // Validate before replacing.
    if (host.currentMusic) host.currentMusic->Stop();
    host.currentMusic.reset();
    Native::PlayOptions options;
    options.bus = "music";
    options.loop = true;
    host.currentMusic = host.audio->Play(source, options);
    return 0;
}
int StopMusic(lua_State* L) {
    Count(L, 0); auto& host = Host(L);
    if (host.currentMusic) host.currentMusic->Stop();
    host.currentMusic.reset(); return 0;
}
int PauseMusic(lua_State* L) {
    Count(L, 0); auto& host = Host(L);
    if (host.currentMusic) host.currentMusic->Pause();
    return 0;
}
int ResumeMusic(lua_State* L) {
    Count(L, 0); auto& host = Host(L);
    if (host.currentMusic) host.currentMusic->Resume();
    return 0;
}
int SetSoundVolume(lua_State* L) {
    Count(L, 1); Host(L).audio->SetBusVolume("sound", Volume(L)); return 0;
}
int GetSoundVolume(lua_State* L) {
    Count(L, 0); lua_pushnumber(L, Host(L).audio->GetBusVolume("sound")); return 1;
}
int SetMusicVolume(lua_State* L) {
    Count(L, 1); Host(L).audio->SetBusVolume("music", Volume(L)); return 0;
}
int GetMusicVolume(lua_State* L) {
    Count(L, 0); lua_pushnumber(L, Host(L).audio->GetBusVolume("music")); return 1;
}
const luaL_Reg Functions[] = {
    {"PlaySound", Guard<PlaySound>}, {"StopSound", Guard<StopSound>},
    {"PauseSound", Guard<PauseSound>}, {"ResumeSound", Guard<ResumeSound>},
    {"PlayMusic", Guard<PlayMusic>}, {"StopMusic", Guard<StopMusic>},
    {"PauseMusic", Guard<PauseMusic>}, {"ResumeMusic", Guard<ResumeMusic>},
    {"SetSoundVolume", Guard<SetSoundVolume>}, {"GetSoundVolume", Guard<GetSoundVolume>},
    {"SetMusicVolume", Guard<SetMusicVolume>}, {"GetMusicVolume", Guard<GetMusicVolume>},
    {nullptr, nullptr}
};
}
struct Context::Impl {
    lua_State* state;
    std::shared_ptr<Token> token;
    Impl(lua_State* L, Native::AudioSystem& audio, Resource::Service& resources)
        : state(L), token(std::make_shared<Token>()) {
        if (!L) throw std::invalid_argument("Audio Context requires a Lua state");
        token->audio = &audio; token->resources = &resources;
    }
};
Context::Context(lua_State* L, Native::AudioSystem& audio, Resource::Service& resources)
    : impl_(std::make_unique<Impl>(L, audio, resources)) {}
Context::~Context() { Shutdown(); }
void Context::Shutdown() noexcept {
    if (!impl_) return;
    // Releasing this reference does not stop the engine-owned voice. Resource
    // retirement and AudioSystem shutdown own deterministic playback stopping.
    impl_->token->audio = nullptr;
    impl_->token->resources = nullptr;
    impl_->token->currentMusic.reset();
}
void Register(lua_State* L, Context& context) {
    if (!L || !context.impl_ || context.impl_->state != L || !context.impl_->token->audio || !context.impl_->token->resources)
        throw std::invalid_argument("Audio.Register requires this Lua state's live Context");
    const int top = lua_gettop(L);
    struct Restore { lua_State* state; int top; ~Restore() { lua_settop(state, top); } } restore{L, top};
    lua_pushlightuserdata(L, &ContextKey); lua_rawget(L, LUA_REGISTRYINDEX);
    if (auto* existing = TrySlot(L, -1); existing && existing->box && existing->box->token->audio &&
        existing->box->token != context.impl_->token)
        throw std::logic_error("only one live Audio Context may register in a Lua state");
    lua_pop(L, 1);
    luaL_newmetatable(L, ServiceType);
    lua_pushcfunction(L, Guard<Collect>); lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "Audio service"); lua_setfield(L, -2, "__metatable"); lua_pop(L, 1);
    auto* slot = new(lua_newuserdata(L, sizeof(Slot))) Slot{};
    luaL_getmetatable(L, ServiceType); lua_setmetatable(L, -2);
    slot->box = new ServiceBox{context.impl_->token};
    const int service = lua_gettop(L);
    lua_pushlightuserdata(L, &ContextKey); lua_pushvalue(L, service); lua_rawset(L, LUA_REGISTRYINDEX);
    lua_newtable(L); const int module = lua_gettop(L);
    for (const auto* function = Functions; function->name; ++function) {
        lua_pushvalue(L, service); lua_pushcclosure(L, function->func, 1); lua_setfield(L, module, function->name);
    }
    lua_pushvalue(L, module); lua_setglobal(L, "Audio");
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_newtable(L); lua_pushvalue(L, -1); lua_setfield(L, LUA_REGISTRYINDEX, "_LOADED"); }
    if (!lua_istable(L, -1)) throw std::invalid_argument("Lua loaded-module registry must be a table");
    lua_pushvalue(L, module); lua_setfield(L, -2, "Audio");
}
}
