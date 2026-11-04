#pragma once
#include "core/modules/Graphic/shader/Compiler.h"
#include <lua.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lua_bind::modules::Graphic::ShaderParams {
enum class TexturePart { Color, Depth, Distance };
struct TextureRequest {
    std::string name;
    bool required{}, requiresCanvas3D{}, provided{};
};
struct TextureSlot {
    std::uint32_t slot{};
    std::size_t texture{}; // Index in Snapshot::textures, never an Image/Atlas slot.
    TexturePart part{TexturePart::Color};
};
struct Uniform {
    std::uint32_t slot{};
    std::vector<std::uint8_t> bytes;
};
struct Stage {
    std::vector<Uniform> params; // Complete zero-initialized Params buffers, ordered by slot.
    std::vector<TextureSlot> textures; // Active stage bindings, ordered by slot.
};
struct Snapshot {
    std::vector<TextureRequest> textures; // All declared logical main textures, in declaration order.
    Stage vertex, fragment;
};
// Called synchronously for EVERY provided declared main texture, including an
// optimized-out one. The absolute stack index is valid only during this call.
// Resolve/validate and retain references only: do not submit draws, mutate params,
// yield, invoke lua_error, allocate Lua objects or leave stack changes. Throw
// C++ errors on failure; use only nonallocating Lua reads of the supplied value.
// Main values with derived declarations must resolve to Canvas3D, even when the
// compiler optimized out the derived texture. No GPU or registry access here.
using ResolveTexture = std::function<void(std::string_view name, int absoluteValueIndex, bool requiresCanvas3D)>;

// Preserves the Lua stack on success/C++ failure; does not call Lua metamethods.
// Numeric data is copied and validated now, not read again when encoding a draw.
// Only Params is packed here; Renderer supplies reflected EngineBuiltins data.
Snapshot Pack(lua_State*, int paramsIndex, const core::modules::Graphic::Shader::Compiled&,
    bool targetThreeD, const ResolveTexture& = {});
}
