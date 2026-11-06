#pragma once
#include <lua.hpp>
#include <exception>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lua_bind {
    // LuaJIT on Windows raises an SEH exception for lua_error. It must not
    // cross an active C++ catch frame (MSVC's unwinder can terminate there).
    // This non-inlined helper ends every C++ exception scope BEFORE Guard
    // raises the Lua error. Argument validation inside it throws C++ errors.
    template<int (*Function)(lua_State*)>
    #ifdef _MSC_VER
    __declspec(noinline)
    #else
    __attribute__((noinline))
    #endif
    int TryFunction(lua_State* L, char* error_buffer, std::size_t capacity) {
        try { return Function(L); }
        catch (const std::exception& error) {
            const std::size_t length = std::min(capacity - 1, std::strlen(error.what()));
            std::memcpy(error_buffer, error.what(), length); error_buffer[length] = 0;
        }
        catch (...) {
            constexpr const char* message = "unknown native exception";
            const std::size_t length = std::min(capacity - 1, std::strlen(message));
            std::memcpy(error_buffer, message, length); error_buffer[length] = 0;
        }
        return -1;
    }
    template<int (*Function)(lua_State*)>
    int Guard(lua_State* L) {
        // Successful calls do not read the diagnostic payload. TryFunction
        // writes and terminates it on every exception; initialize only the
        // empty-message sentinel for a function returning a negative result.
        char error_buffer[4096];
        error_buffer[0] = 0;
        const int results = TryFunction<Function>(L, error_buffer, sizeof(error_buffer));
        if (results >= 0) return results;
        // Even allocating the Lua error string happens after all catch scopes.
        lua_pushstring(L, error_buffer);
        return lua_error(L);
    }
    inline std::string_view CheckString(lua_State* L, int index) {
        if (lua_type(L, index) != LUA_TSTRING)
            throw std::invalid_argument("argument #" + std::to_string(index) + " must be a string");
        std::size_t size{};
        const char* data = lua_tolstring(L, index, &size);
        return {data, size};
    }
    inline double OptionalNumber(lua_State* L, int index, double fallback) {
        if (lua_isnoneornil(L, index)) return fallback;
        if (lua_type(L, index) != LUA_TNUMBER)
            throw std::invalid_argument("argument #" + std::to_string(index) + " must be a number");
        return lua_tonumber(L, index);
    }
}
