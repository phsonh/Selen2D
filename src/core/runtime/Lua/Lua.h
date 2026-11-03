#pragma once
#include <lua.hpp>
#include <string>

namespace core::modules::File { class FileSystem; }

namespace core::runtime::Lua
{
    bool Init();

    lua_State* GetState();

    bool DoFile(
        const std::string& path,
        const core::modules::File::FileSystem& files,
        std::string& error
    );

    bool SetFrameFunction(lua_State* caller, int index);
    bool SetExitFunction(lua_State* caller, int index);
    bool HasFrameFunction();
    bool CallFrameFunction(std::string& error);
    bool CallExitFunction(std::string& error);

    void Shutdown();
}
