#pragma once
#include "core/modules/Graphic/geometry/Mesh.h"
#include <lua.hpp>

namespace lua_bind::modules::Graphic::MeshBinding {
using Mesh = core::modules::Graphic::Geometry::MeshData;
// Register only the opaque userdata metatable. No global/module is created.
void Register(lua_State*);
// Lua callback body: vertices, indices -> immutable Mesh userdata. The caller
// registers Guard<Create> to translate native exceptions at the Lua boundary.
int Create(lua_State*);
void Push(lua_State*, const std::shared_ptr<const Mesh>&);
bool Is(lua_State*, int index);
std::shared_ptr<const Mesh> Pin(lua_State*, int index);
}
