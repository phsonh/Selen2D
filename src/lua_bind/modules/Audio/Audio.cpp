#include "Audio.h"
#include "core/modules/Audio/Audio.h"
#include "core/modules/Resource/AudioAsset.h"
#include "lua_bind/Guard.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lua_bind::modules::Audio {
namespace {
namespace Native = core::modules::Audio;
namespace Resource = core::modules::Resource;
constexpr const char* ServiceType = "engine.Audio.Service";
constexpr const char* PlayerType = "engine.Audio.Player";
char ContextKey;
struct PlayerBox;
struct Token {
    lua_State* state{};
    Native::AudioSystem* audio{};
    Resource::Service* resources{};
    int selected{LUA_NOREF};
    // The registry reference owns the userdata. This pointer is only an identity
    // and lookup shortcut; unselected players have no service-owned strong root.
    PlayerBox* selectedPlayer{};
};
struct ServiceBox {
    static constexpr std::uint64_t tag = 0x415544494f535256ULL;
    std::shared_ptr<Token> token;
};
struct PlayerBox {
    static constexpr std::uint64_t tag = 0x415544494f504c52ULL;
    std::shared_ptr<Token> token;
    std::shared_ptr<Native::Player> player;
};
template<class Box> struct Slot { std::uint64_t tag{Box::tag}; Box* value{}; };

template<class Box> Box** TrySlot(lua_State* L, int index, const char* type) {
    if (lua_type(L, index) != LUA_TUSERDATA || lua_objlen(L, index) != sizeof(Slot<Box>) ||
        !lua_getmetatable(L, index)) return nullptr;
    luaL_getmetatable(L, type);
    const bool matches = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    if (!matches) return nullptr;
    auto* slot = static_cast<Slot<Box>*>(lua_touserdata(L, index));
    return slot->tag == Box::tag ? &slot->value : nullptr;
}
template<class Box> Box& RequireBox(lua_State* L, int index, const char* type) {
    auto** slot = TrySlot<Box>(L, index, type);
    if (!slot || !*slot) throw std::invalid_argument("invalid or released Audio handle");
    return **slot;
}
template<class Box> Box** NewSlot(lua_State* L, const char* type) {
    auto* slot = new(lua_newuserdata(L, sizeof(Slot<Box>))) Slot<Box>{};
    luaL_getmetatable(L, type); lua_setmetatable(L, -2);
    return &slot->value;
}
void ClearSelection(lua_State* L, Token& token) noexcept {
    const int previous = token.selected;
    token.selected = LUA_NOREF;
    token.selectedPlayer = nullptr;
    if (previous != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, previous);
}
void DestroyPlayer(lua_State* L, PlayerBox& box) noexcept {
    if (box.player) box.player->Destroy();
    if (box.token && box.token->selectedPlayer == &box) ClearSelection(L, *box.token);
}
int CollectService(lua_State* L) {
    if (auto** slot = TrySlot<ServiceBox>(L, 1, ServiceType)) { delete *slot; *slot = nullptr; }
    return 0;
}
int CollectPlayer(lua_State* L) {
    if (auto** slot = TrySlot<PlayerBox>(L, 1, PlayerType); slot && *slot) {
        DestroyPlayer(L, **slot);
        delete *slot; *slot = nullptr;
    }
    return 0;
}
void Count(lua_State* L, int minimum, int maximum) {
    const int count = lua_gettop(L);
    if (count < minimum || count > maximum) throw std::invalid_argument("Audio: invalid argument count");
    if (!lua_checkstack(L, 8)) throw std::runtime_error("Audio Lua stack capacity exceeded");
}
Token& LiveToken(const std::shared_ptr<Token>& token) {
    if (!token || !token->audio || !token->resources)
        throw std::runtime_error("Audio Context has shut down");
    return *token;
}
ServiceBox& Module(lua_State* L) { return RequireBox<ServiceBox>(L, lua_upvalueindex(1), ServiceType); }
Token& Host(lua_State* L) { return LiveToken(Module(L).token); }
PlayerBox& LivePlayer(PlayerBox& box) {
    (void)LiveToken(box.token);
    if (!box.player || box.player->IsDestroyed()) throw std::runtime_error("Audio player has been destroyed");
    return box;
}
template<bool Object> PlayerBox& Target(lua_State* L) {
    if constexpr (Object) return LivePlayer(RequireBox<PlayerBox>(L, 1, PlayerType));
    else {
        auto& host = Host(L);
        if (!host.selectedPlayer) throw std::runtime_error("Audio has no current player");
        return LivePlayer(*host.selectedPlayer);
    }
}
struct NamedSource { std::string name; std::shared_ptr<Native::Source> source; };
NamedSource Source(lua_State* L, Token& host, int index) {
    const auto text = CheckString(L, index);
    if (text.empty() || text.size() > 512 || text.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Audio resource name must be nonempty, at most 512 bytes and contain no NUL");
    const std::string name(text);
    const auto handle = host.resources->Find(name);
    if (!handle) throw std::runtime_error("audio resource does not exist or is not published: " + name);
    const auto kind = host.resources->GetInfo(*handle).id.kind;
    if (kind != Resource::Kind::Sound && kind != Resource::Kind::Music)
        throw std::invalid_argument("audio resource must be Sound or Music: " + name);
    const auto asset = std::dynamic_pointer_cast<const Resource::AudioAsset>(host.resources->Acquire(*handle));
    if (!asset || !asset->source) throw std::runtime_error("audio resource has no prepared source: " + name);
    return {name, asset->source};
}
double Number(lua_State* L, int index, double minimum, double maximum, const char* label) {
    if (lua_type(L, index) != LUA_TNUMBER)
        throw std::invalid_argument(std::string("Audio ") + label + " must be a number");
    const double value = lua_tonumber(L, index);
    if (!std::isfinite(value) || value < minimum || value > maximum)
        throw std::invalid_argument(std::string("Audio ") + label + " must be finite and within its allowed range");
    return value;
}
int CreatePlayer(lua_State* L) {
    Count(L, 0, 0);
    auto& service = Module(L);
    auto& host = LiveToken(service.token);
    auto** slot = NewSlot<PlayerBox>(L, PlayerType);
    auto box = std::make_unique<PlayerBox>();
    box->token = service.token;
    box->player = host.audio->CreatePlayer();
    if (!box->player) throw std::runtime_error("Audio did not create a player");
    *slot = box.release();
    return 1;
}
int SetPlayer(lua_State* L) {
    Count(L, 1, 1);
    auto& host = Host(L);
    if (lua_isnil(L, 1)) { ClearSelection(L, host); return 0; }
    auto& box = LivePlayer(RequireBox<PlayerBox>(L, 1, PlayerType));
    if (box.token.get() != &host) throw std::invalid_argument("Audio player belongs to another Context");
    // Validate and acquire the replacement root before releasing the old one.
    lua_pushvalue(L, 1);
    const int selected = luaL_ref(L, LUA_REGISTRYINDEX);
    ClearSelection(L, host);
    host.selected = selected;
    host.selectedPlayer = &box;
    return 0;
}
int GetPlayer(lua_State* L) {
    Count(L, 0, 0);
    auto& host = Host(L);
    if (host.selected == LUA_NOREF) lua_pushnil(L);
    else lua_rawgeti(L, LUA_REGISTRYINDEX, host.selected);
    return 1;
}
template<bool Object> int Play(lua_State* L) {
    constexpr int offset = Object ? 1 : 0;
    Count(L, offset + 1, offset + 1);
    auto& box = Target<Object>(L);
    const auto source = Source(L, *box.token, offset + 1);
    box.player->Play(source.name, source.source);
    return 0;
}
enum class Control { Stop, Pause, Resume };
template<bool Object, Control Operation> int ControlPlayer(lua_State* L) {
    constexpr int offset = Object ? 1 : 0;
    Count(L, offset, offset + 1);
    auto& box = Target<Object>(L);
    // Even when this player has no voice, a supplied name must resolve to a
    // published audio resource. Only omitted/nil means every voice.
    const auto source = lua_isnoneornil(L, offset + 1) ? std::shared_ptr<Native::Source>{} :
        Source(L, *box.token, offset + 1).source;
    if constexpr (Operation == Control::Stop) box.player->Stop(source);
    if constexpr (Operation == Control::Pause) box.player->Pause(source);
    if constexpr (Operation == Control::Resume) box.player->Resume(source);
    return 0;
}
template<bool Object, bool Pan> int SetGain(lua_State* L) {
    constexpr int offset = Object ? 1 : 0;
    Count(L, offset + 1, offset + 1);
    auto& box = Target<Object>(L);
    const double value = Number(L, offset + 1, Pan ? -1.0 : 0.0, 1.0, Pan ? "pan" : "volume");
    if constexpr (Pan) box.player->SetPan(value);
    else box.player->SetVolume(value);
    return 0;
}
template<bool Object, bool Pan> int GetGain(lua_State* L) {
    constexpr int offset = Object ? 1 : 0;
    Count(L, offset, offset);
    auto& box = Target<Object>(L);
    lua_pushnumber(L, Pan ? box.player->GetPan() : box.player->GetVolume());
    return 1;
}
template<bool Object> int GetPlaying(lua_State* L) {
    constexpr int offset = Object ? 1 : 0;
    Count(L, offset, offset);
    const auto names = Target<Object>(L).player->GetPlaying();
    if (names.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("too many Audio names for a Lua array");
    lua_createtable(L, static_cast<int>(names.size()), 0);
    int index = 0;
    for (const auto& name : names) {
        lua_pushlstring(L, name.data(), name.size());
        lua_rawseti(L, -2, ++index);
    }
    return 1;
}
template<bool Object> int IsPlaying(lua_State* L) {
    constexpr int offset = Object ? 1 : 0;
    Count(L, offset + 1, offset + 1);
    auto& box = Target<Object>(L);
    const auto source = Source(L, *box.token, offset + 1);
    lua_pushboolean(L, box.player->IsPlaying(source.source));
    return 1;
}
int Destroy(lua_State* L) {
    Count(L, 1, 1);
    auto& box = RequireBox<PlayerBox>(L, 1, PlayerType);
    (void)LiveToken(box.token);
    DestroyPlayer(L, box); // Repeated explicit Destroy remains valid and empty.
    return 0;
}
const luaL_Reg Functions[] = {
    {"CreatePlayer", Guard<CreatePlayer>}, {"SetPlayer", Guard<SetPlayer>}, {"GetPlayer", Guard<GetPlayer>},
    {"Play", Guard<Play<false>>},
    {"Stop", Guard<ControlPlayer<false, Control::Stop>>},
    {"Pause", Guard<ControlPlayer<false, Control::Pause>>},
    {"Resume", Guard<ControlPlayer<false, Control::Resume>>},
    {"SetVolume", Guard<SetGain<false, false>>}, {"GetVolume", Guard<GetGain<false, false>>},
    {"SetPan", Guard<SetGain<false, true>>}, {"GetPan", Guard<GetGain<false, true>>},
    {"GetPlaying", Guard<GetPlaying<false>>}, {"IsPlaying", Guard<IsPlaying<false>>},
    {nullptr, nullptr}
};
const luaL_Reg Methods[] = {
    {"Play", Guard<Play<true>>},
    {"Stop", Guard<ControlPlayer<true, Control::Stop>>},
    {"Pause", Guard<ControlPlayer<true, Control::Pause>>},
    {"Resume", Guard<ControlPlayer<true, Control::Resume>>},
    {"SetVolume", Guard<SetGain<true, false>>}, {"GetVolume", Guard<GetGain<true, false>>},
    {"SetPan", Guard<SetGain<true, true>>}, {"GetPan", Guard<GetGain<true, true>>},
    {"GetPlaying", Guard<GetPlaying<true>>}, {"IsPlaying", Guard<IsPlaying<true>>},
    {"Destroy", Guard<Destroy>}, {nullptr, nullptr}
};
}
struct Context::Impl {
    lua_State* state;
    std::shared_ptr<Token> token;
    Impl(lua_State* L, Native::AudioSystem& audio, Resource::Service& resources)
        : state(L), token(std::make_shared<Token>()) {
        if (!L) throw std::invalid_argument("Audio Context requires a Lua state");
        token->state = L; token->audio = &audio; token->resources = &resources;
    }
};
Context::Context(lua_State* L, Native::AudioSystem& audio, Resource::Service& resources)
    : impl_(std::make_unique<Impl>(L, audio, resources)) {}
Context::~Context() { Shutdown(); }
void Context::Shutdown() noexcept {
    if (!impl_ || !impl_->token->state) return;
    ClearSelection(impl_->token->state, *impl_->token);
    impl_->token->state = nullptr;
    impl_->token->audio = nullptr;
    impl_->token->resources = nullptr;
    // Engine-owned StopAllPlayersAndDrain stops all players before resources,
    // VFS and the device retire. Lua GC is only an independent fallback.
}
void Register(lua_State* L, Context& context) {
    if (!L || !context.impl_ || context.impl_->state != L || !context.impl_->token->audio || !context.impl_->token->resources)
        throw std::invalid_argument("Audio.Register requires this Lua state's live Context");
    const int top = lua_gettop(L);
    struct Restore { lua_State* state; int top; ~Restore() { lua_settop(state, top); } } restore{L, top};
    lua_pushlightuserdata(L, &ContextKey); lua_rawget(L, LUA_REGISTRYINDEX);
    if (auto** existing = TrySlot<ServiceBox>(L, -1, ServiceType); existing && *existing &&
        (*existing)->token->audio && (*existing)->token != context.impl_->token)
        throw std::logic_error("only one live Audio Context may register in a Lua state");
    lua_pop(L, 1);
    luaL_newmetatable(L, ServiceType);
    lua_pushcfunction(L, Guard<CollectService>); lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "Audio service"); lua_setfield(L, -2, "__metatable"); lua_pop(L, 1);
    luaL_newmetatable(L, PlayerType);
    lua_pushcfunction(L, Guard<CollectPlayer>); lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "Audio player"); lua_setfield(L, -2, "__metatable");
    lua_newtable(L);
    for (const auto* method = Methods; method->name; ++method) {
        lua_pushcfunction(L, method->func); lua_setfield(L, -2, method->name);
    }
    lua_setfield(L, -2, "__index"); lua_pop(L, 1);
    auto** slot = NewSlot<ServiceBox>(L, ServiceType);
    *slot = new ServiceBox{context.impl_->token};
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
