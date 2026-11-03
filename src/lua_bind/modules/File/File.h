#pragma once
#include <lua.hpp>
#include <memory>
namespace core::modules::File { class FileSystem; }
namespace lua_bind::modules::File {
void Register(lua_State* L, std::shared_ptr<const core::modules::File::FileSystem> filesystem);
}
