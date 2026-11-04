#pragma once
#include "core/modules/Graphic/geometry/Mesh.h"
#include <lua.hpp>

namespace lua_bind::modules::Graphic::MeshBinding {
using Mesh = core::modules::Graphic::Geometry::MeshData;
// Optional host validity for product-created wrappers. Native MeshData and
// already pinned snapshots do not depend on a live Lua/renderer service.
struct Owner {
    std::shared_ptr<void> lifetime;
    void (*check)(const std::shared_ptr<void>&){};
    void Check() const { if(check) check(lifetime); }
};
// Register only the opaque userdata metatable. No global/module is created.
void Register(lua_State*);
// Lua callback body: vertices, indices -> Mesh wrapper holding an immutable
// data version. Update replaces that pointer; submitted draws retain their Pin.
// The caller
// registers Guard<Create> to translate native exceptions at the Lua boundary.
int Create(lua_State*);
int CreateOwned(lua_State*, Owner);
int Update(lua_State*);
void Push(lua_State*, const std::shared_ptr<const Mesh>&, Owner = {});
bool Is(lua_State*, int index);
std::shared_ptr<const Mesh> Pin(lua_State*, int index);
}
