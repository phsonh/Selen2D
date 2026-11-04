#pragma once
#include <lua.hpp>
#include <memory>
namespace core::modules::File { class FileSystem; }
namespace core::runtime::Lua {
void InstallStandardIo(lua_State*,std::shared_ptr<const modules::File::FileSystem>);
}
