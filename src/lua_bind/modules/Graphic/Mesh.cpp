#include "Mesh.h"
#include "lua_bind/Guard.h"
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace lua_bind::modules::Graphic::MeshBinding {
namespace {
namespace G = core::modules::Graphic::Geometry;
constexpr const char* Type = "Solongo.Graphic.Mesh";
constexpr std::uint64_t Tag = 0x53474D4553483031ULL;
struct Slot { std::uint64_t tag{Tag}; std::shared_ptr<const Mesh>* value{}; };
void Stack(lua_State* L) {
    if (!L || !lua_checkstack(L, 12)) throw std::runtime_error("Mesh requires a valid Lua state with available stack space");
}
int Absolute(lua_State* L, int index) {
    return index > 0 || index <= LUA_REGISTRYINDEX ? index : lua_gettop(L) + index + 1;
}
Slot* Try(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TUSERDATA || lua_objlen(L, index) != sizeof(Slot) || !lua_getmetatable(L, index)) return nullptr;
    luaL_getmetatable(L, Type);
    const bool matches = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    if (!matches) return nullptr;
    auto* slot = static_cast<Slot*>(lua_touserdata(L, index));
    return slot->tag == Tag ? slot : nullptr;
}
int Collect(lua_State* L) {
    if (auto* slot = Try(L, 1)) { delete slot->value; slot->value = nullptr; }
    return 0;
}
Slot* NewSlot(lua_State* L) {
    Register(L);
    auto* slot = new(lua_newuserdata(L, sizeof(Slot))) Slot{};
    luaL_getmetatable(L, Type); lua_setmetatable(L, -2);
    return slot;
}
std::size_t Dense(lua_State* L, int index, std::size_t maximum, const char* label) {
    index = Absolute(L, index);
    if (lua_type(L, index) != LUA_TTABLE) throw std::invalid_argument(std::string(label) + " must be an ordinary array table");
    if (lua_getmetatable(L, index)) {
        lua_pop(L, 1);
        throw std::invalid_argument(std::string(label) + " must not have a metatable");
    }
    const auto count = lua_objlen(L, index);
    if (count > maximum) throw std::length_error(std::string(label) + " exceeds Mesh capacity");
    std::size_t found{};
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (lua_type(L, -2) != LUA_TNUMBER) throw std::invalid_argument(std::string(label) + " has a non-array key");
        const auto key = lua_tonumber(L, -2);
        if (!std::isfinite(key) || key < 1 || key > count || std::floor(key) != key)
            throw std::invalid_argument(std::string(label) + " has a hole or extra array key");
        ++found;
        lua_pop(L, 1);
    }
    if (found != count) throw std::invalid_argument(std::string(label) + " must be dense without missing elements");
    return count;
}
double Number(lua_State* L, int index, const char* label) {
    if (lua_type(L, index) != LUA_TNUMBER || !std::isfinite(lua_tonumber(L, index)))
        throw std::invalid_argument(std::string(label) + " must be a finite number");
    return lua_tonumber(L, index);
}
double Component(lua_State* L, int table, int component) {
    lua_rawgeti(L, table, component);
    const auto value = Number(L, -1, "Mesh vertex component");
    lua_pop(L, 1);
    return value;
}
}
void Register(lua_State* L) {
    Stack(L);
    if (luaL_newmetatable(L, Type)) {
        lua_pushcfunction(L, Guard<Collect>); lua_setfield(L, -2, "__gc");
        lua_pushstring(L, Type); lua_setfield(L, -2, "__metatable");
        lua_newtable(L); lua_setfield(L, -2, "__index"); // No Update/Destroy or mutable properties.
    }
    lua_pop(L, 1);
}
void Push(lua_State* L, const std::shared_ptr<const Mesh>& mesh) {
    if (!mesh) throw std::invalid_argument("cannot push a null Mesh");
    auto* slot = NewSlot(L);
    slot->value = new std::shared_ptr<const Mesh>(mesh);
}
bool Is(lua_State* L, int index) {
    Stack(L);
    const auto* slot = Try(L, index);
    return slot && slot->value && *slot->value;
}
std::shared_ptr<const Mesh> Pin(lua_State* L, int index) {
    Stack(L);
    const auto* slot = Try(L, index);
    if (!slot || !slot->value || !*slot->value) throw std::invalid_argument("expected a live Graphic Mesh userdata");
    return *slot->value;
}
int Create(lua_State* L) {
    Stack(L);
    if (lua_gettop(L) != 2) throw std::invalid_argument("Graphic.Mesh expects vertices and indices");
    // Allocate the GC shell before owning any native buffers. Parsing uses only
    // raw Lua access with reserved stack space, never metamethods or callbacks.
    auto* slot = NewSlot(L);
    const G::MeshLimits limits;
    const auto vertexCount = Dense(L, 1, limits.maxVertices, "Mesh vertices");
    const auto indexCount = Dense(L, 2, limits.maxIndices, "Mesh indices");
    if (vertexCount < 3 || indexCount < 3 || indexCount % 3)
        throw std::invalid_argument("Mesh requires at least three vertices and a nonempty triangle index list");
    std::vector<G::MeshInputVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(vertexCount); indices.reserve(indexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        lua_rawgeti(L, 1, static_cast<int>(i + 1));
        const auto table = lua_gettop(L);
        if (Dense(L, table, 6, "Mesh vertex") != 6)
            throw std::invalid_argument("Mesh vertex must contain exactly x,y,z,u,v,color");
        G::MeshInputVertex vertex;
        vertex.x = Component(L, table, 1); vertex.y = Component(L, table, 2); vertex.z = Component(L, table, 3);
        vertex.u = Component(L, table, 4); vertex.v = Component(L, table, 5);
        const auto color = Component(L, table, 6);
        if (color < 0 || color > std::numeric_limits<std::uint32_t>::max() || std::floor(color) != color)
            throw std::invalid_argument("Mesh color must be an unsigned 32-bit ARGB integer");
        vertex.color = static_cast<std::uint32_t>(color);
        vertices.push_back(vertex); lua_pop(L, 1);
    }
    for (std::size_t i = 0; i < indexCount; ++i) {
        lua_rawgeti(L, 2, static_cast<int>(i + 1));
        const auto index = Number(L, -1, "Mesh index");
        if (index < 1 || index > vertexCount || std::floor(index) != index)
            throw std::invalid_argument("Mesh indices must be integers in [1,vertexCount]");
        indices.push_back(static_cast<std::uint32_t>(index - 1)); lua_pop(L, 1);
    }
    slot->value = new std::shared_ptr<const Mesh>(G::MeshData::Create(vertices, indices, limits));
    return 1;
}
}
