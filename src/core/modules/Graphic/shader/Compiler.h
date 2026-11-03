#pragma once
#include "Model.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace core::modules::File { class FileSystem; }
namespace core::modules::Graphic::Shader {
enum class BytecodeFormat { Dxil };
struct Field {
    std::string name;
    ValueType type{};
    std::uint32_t offset{}, size{};
    bool used{};
};
struct BufferBinding {
    std::string name;
    std::uint32_t slot{}, size{};
    std::vector<Field> fields;
};
struct TextureBinding {
    std::string name;
    std::uint32_t slot{};
    // Private generated inputs participate in the same dense stage slots, but
    // are supplied by Renderer, never by the user's Lua params table.
    bool engine{};
};
struct Stage {
    std::vector<std::uint8_t> code;
    std::string entry;
    std::vector<TextureBinding> textures;
    std::vector<BufferBinding> buffers;
};
struct Compiled {
    BytecodeFormat format{BytecodeFormat::Dxil};
    std::string path;
    Model model; // Full declared field set survives optimization for Lua errors.
    Stage vertex, fragment;
    std::size_t ResidentBytes() const noexcept;
};
// CPU-only, no Lua, registry, Renderer or SDL_GPU device access. Cancellation is
// checked between compiler calls; DXC itself cannot be interrupted safely.
Compiled Compile(std::shared_ptr<const File::FileSystem>, std::string_view path,
    const std::function<bool()>& cancelled = {});
}
