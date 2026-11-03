#include "Math.h"
#include "core/modules/Math/Math.h"
#include "lua_bind/Guard.h"
#include <cmath>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace lua_bind::modules::Math {
namespace {
namespace Native = core::modules::Math;
using Generator = Native::Random::Generator;
constexpr char GeneratorType[] = "engine.Math.Generator";
constexpr char Algorithm[] = "xorshift128-11-8-19";
char currentGeneratorKey;
static_assert(std::is_trivially_destructible_v<Generator>);

void Arity(lua_State* L, int count, const char* name) {
    if (lua_gettop(L) != count)
        throw std::invalid_argument(std::string(name) + " expects exactly " + std::to_string(count) + " arguments");
}
double Number(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TNUMBER)
        throw std::invalid_argument("argument #" + std::to_string(index) + " must be a number");
    return lua_tonumber(L, index);
}
Generator& Require(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TUSERDATA || lua_objlen(L, index) != sizeof(Generator) ||
        !lua_getmetatable(L, index))
        throw std::invalid_argument("expected a Math.Random generator");
    luaL_getmetatable(L, GeneratorType);
    const bool valid = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    if (!valid) throw std::invalid_argument("expected a Math.Random generator");
    return *static_cast<Generator*>(lua_touserdata(L, index));
}
Generator& Current(lua_State* L) {
    lua_pushlightuserdata(L, &currentGeneratorKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto& value = Require(L, -1);
    lua_pop(L, 1);
    return value; // The private registry entry retains the exact userdata.
}
void Push(lua_State* L, const Generator& value) {
    void* memory = lua_newuserdata(L, sizeof(Generator));
    new (memory) Generator(value); // Copy complete state, without seeding/drawing.
    luaL_getmetatable(L, GeneratorType);
    lua_setmetatable(L, -2);
}
template<bool Object>
Generator& Self(lua_State* L, int arguments, const char* name) {
    Arity(L, arguments + (Object ? 1 : 0), name);
    return Object ? Require(L, 1) : Current(L);
}
template<double (*Function)(double)>
int Unary(lua_State* L) {
    Arity(L, 1, "Math trigonometric function");
    lua_pushnumber(L, Function(Number(L, 1)));
    return 1;
}
int Atan2(lua_State* L) {
    Arity(L, 2, "Math.atan2");
    lua_pushnumber(L, Native::atan2(Number(L, 1), Number(L, 2)));
    return 1;
}
int Create(lua_State* L) {
    if (lua_gettop(L) > 1) throw std::invalid_argument("Math.Random.Create expects at most one seed");
    const Generator value(lua_isnoneornil(L, 1) ? 0 : Number(L, 1));
    Push(L, value);
    return 1;
}
int SetGenerator(lua_State* L) {
    Arity(L, 1, "Math.Random.SetGenerator");
    (void)Require(L, 1);
    lua_pushlightuserdata(L, &currentGeneratorKey);
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}
int GetGenerator(lua_State* L) {
    Arity(L, 0, "Math.Random.GetGenerator");
    lua_pushlightuserdata(L, &currentGeneratorKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    (void)Require(L, -1);
    return 1;
}
template<bool Object>
int Seed(lua_State* L) {
    auto& value = Self<Object>(L, 1, "Random.Seed");
    value.Seed(Number(L, Object ? 2 : 1));
    return 0;
}
template<bool Object>
int GetSeed(lua_State* L) {
    const auto& value = Self<Object>(L, 0, "Random.GetSeed");
    lua_pushnumber(L, value.GetSeed());
    return 1;
}
template<bool Object>
int Int(lua_State* L) {
    auto& value = Self<Object>(L, 2, "Random.Int");
    const int first = Object ? 2 : 1;
    lua_pushnumber(L, static_cast<lua_Number>(value.Int(Number(L, first), Number(L, first + 1))));
    return 1;
}
template<bool Object>
int Float(lua_State* L) {
    auto& value = Self<Object>(L, 2, "Random.Float");
    const int first = Object ? 2 : 1;
    lua_pushnumber(L, value.Float(Number(L, first), Number(L, first + 1)));
    return 1;
}
template<bool Object>
int Sign(lua_State* L) {
    lua_pushinteger(L, Self<Object>(L, 0, "Random.Sign").Sign());
    return 1;
}
template<bool Object>
int GetState(lua_State* L) {
    const auto state = Self<Object>(L, 0, "Random.GetState").GetState();
    lua_createtable(L, 0, 6);
    lua_pushliteral(L, "xorshift128-11-8-19"); lua_setfield(L, -2, "algorithm");
    const auto field = [&](const char* name, std::uint32_t number) {
        lua_pushnumber(L, number); lua_setfield(L, -2, name);
    };
    field("seed", state.seed); field("x", state.x); field("y", state.y); field("z", state.z); field("w", state.w);
    return 1;
}
std::uint32_t Word(lua_State* L, int index, std::string_view name) {
    if (lua_type(L, index) != LUA_TNUMBER)
        throw std::invalid_argument("Random.SetState." + std::string(name) + " must be a uint32 number");
    const auto value = lua_tonumber(L, index);
    if (!std::isfinite(value) || value < 0 || value > 4294967295.0 || std::trunc(value) != value)
        throw std::invalid_argument("Random.SetState." + std::string(name) + " must be a uint32 integer");
    return static_cast<std::uint32_t>(value);
}
template<bool Object>
int SetState(lua_State* L) {
    auto& value = Self<Object>(L, 1, "Random.SetState");
    const int index = Object ? 2 : 1;
    if (lua_type(L, index) != LUA_TTABLE) throw std::invalid_argument("Random.SetState requires a table");
    Native::Random::State state{};
    unsigned fields{};
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (lua_type(L, -2) != LUA_TSTRING) throw std::invalid_argument("Random.SetState field keys must be strings");
        const auto name = CheckString(L, -2);
        if (name == "algorithm") {
            if (CheckString(L, -1) != Algorithm) throw std::invalid_argument("Random.SetState algorithm must be xorshift128-11-8-19");
            fields |= 1;
        } else if (name == "seed") { state.seed = Word(L, -1, name); fields |= 2; }
        else if (name == "x") { state.x = Word(L, -1, name); fields |= 4; }
        else if (name == "y") { state.y = Word(L, -1, name); fields |= 8; }
        else if (name == "z") { state.z = Word(L, -1, name); fields |= 16; }
        else if (name == "w") { state.w = Word(L, -1, name); fields |= 32; }
        else throw std::invalid_argument("Random.SetState contains an unknown field");
        lua_pop(L, 1);
    }
    if (fields != 63) throw std::invalid_argument("Random.SetState requires exactly algorithm, seed, x, y, z, w");
    value.SetState(state); // No mutation occurs before every own field is valid.
    return 0;
}
template<bool Object>
int Clone(lua_State* L) {
    const auto state = Self<Object>(L, 0, "Random.Clone");
    Push(L, state);
    return 1;
}
const luaL_Reg Functions[] = {
    {"sin", Guard<Unary<Native::sin>>}, {"cos", Guard<Unary<Native::cos>>},
    {"tan", Guard<Unary<Native::tan>>}, {"asin", Guard<Unary<Native::asin>>},
    {"acos", Guard<Unary<Native::acos>>}, {"atan2", Guard<Atan2>}, {nullptr, nullptr}
};
const luaL_Reg RandomFunctions[] = {
    {"Create", Guard<Create>}, {"SetGenerator", Guard<SetGenerator>}, {"GetGenerator", Guard<GetGenerator>},
    {"Seed", Guard<Seed<false>>}, {"GetSeed", Guard<GetSeed<false>>}, {"Int", Guard<Int<false>>},
    {"Float", Guard<Float<false>>}, {"Sign", Guard<Sign<false>>}, {"GetState", Guard<GetState<false>>},
    {"SetState", Guard<SetState<false>>}, {"Clone", Guard<Clone<false>>}, {nullptr, nullptr}
};
const luaL_Reg Methods[] = {
    {"Seed", Guard<Seed<true>>}, {"GetSeed", Guard<GetSeed<true>>}, {"Int", Guard<Int<true>>},
    {"Float", Guard<Float<true>>}, {"Sign", Guard<Sign<true>>}, {"GetState", Guard<GetState<true>>},
    {"SetState", Guard<SetState<true>>}, {"Clone", Guard<Clone<true>>}, {nullptr, nullptr}
};
}
void Register(lua_State* L) {
    luaL_newmetatable(L, GeneratorType);
    lua_newtable(L); luaL_register(L, nullptr, Methods); lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "Math.Random generator"); lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    lua_pushlightuserdata(L, &currentGeneratorKey);
    Push(L, Generator(0));
    lua_rawset(L, LUA_REGISTRYINDEX);
    luaL_register(L, "Math", Functions);
    luaL_register(L, "Math.Random", RandomFunctions);
    lua_pop(L, 2);
}
}
