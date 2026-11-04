#pragma once
#include <SDL3/SDL_gpu.h>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include "core/modules/Graphic/geometry/Quad.h"

namespace core::modules::Graphic::Render {
enum class Filter { Point, Linear };
enum class Address { Clamp, Repeat };
enum class Blend : std::uint8_t {
    MulAlpha, MulAdd, MulSub, MulRev, MulMin, MulMax, MulMul, MulScreen,
    AddAlpha, AddAdd, AddSub, AddRev, AddMin, AddMax, AddMul, AddScreen,
    AlphaBalance, Replace
};
constexpr bool IsValidBlend(Blend value) noexcept { return value >= Blend::MulAlpha && value <= Blend::Replace; }
constexpr bool UsesAddColor(Blend value) noexcept { return value >= Blend::AddAlpha && value <= Blend::AddScreen; }
// Only the 18 formal spellings. No empty-string, "one", or abbreviated aliases.
std::optional<Blend> ParseBlend(std::string_view) noexcept;
std::string_view BlendName(Blend); // Invalid native enum values are errors.
enum class Alpha { Straight, Premultiplied };

// Native resolved snapshot, never a Lua Texture resource. The binding/provider
// obtains authorization and supplies a strong backing pin before calling Renderer.
// Region and anchor use source pixels; origin is top-left, V points down.
struct Image {
    SDL_GPUTexture* texture{};
    std::shared_ptr<const void> lifetime;
    std::uint32_t textureWidth{}, textureHeight{}, x{}, y{}, width{}, height{};
    // Integer coverage is storage/copy metadata. Fractional logical crops do
    // not change it; zero logical extent means the full coverage (native Canvas).
    double insetX{}, insetY{}, logicalWidth{}, logicalHeight{};
    double LogicalWidth() const noexcept { return logicalWidth > 0 ? logicalWidth : width; }
    double LogicalHeight() const noexcept { return logicalHeight > 0 ? logicalHeight : height; }
    std::uint32_t textureLevels{1}; // Physical backing levels; packed Atlas strips always have one.
    double anchorX{}, anchorY{}, scaleX{1}, scaleY{1};
    Filter filter{Filter::Linear};
    Address address{Address::Clamp};
    Alpha alpha{Alpha::Straight};
    // Zero uses the existing physical texture mip chain. Otherwise each logical
    // mip is a separately extruded region in one vertical strip at physical LOD 0.
    // x/y identify level zero's content; subsequent levels retain x and advance
    // y by the preceding level's height plus twice mipExtrusion.
    std::uint32_t mipLevels{}, mipExtrusion{2};
};
struct Paint { std::uint32_t color{0xFFFFFFFF}; Blend blend{Blend::MulAlpha}; double layer{}; };
struct Transform { double x{}, y{}, rotation{}, scaleX{1}, scaleY{1}; };
// Image-local normalized UV (0..1); metadata scale/anchor does not modify geometry.
struct QuadVertex { double x{},y{},u{},v{}; std::uint32_t color{0xFFFFFFFF}; };
using QuadVertices=std::array<QuadVertex,4>;
struct QuadVertex3 { double x{},y{},z{},u{},v{}; std::uint32_t color{0xFFFFFFFF}; };
using QuadVertices3=std::array<QuadVertex3,4>;

namespace detail {
// Fixed shader layout. Vertex buffers are shared per frame slot, not per sprite.
struct Vertex {
    float x{}, y{}, z{}, w{1}, u{}, v{}, q{1};
    float r{}, g{}, b{}, a{};
    float minU{}, minV{}, maxU{}, maxV{};
    // sampling: 0=local clamp, 1=whole-texture repeat, 2=local point repeat,
    // 3=local linear repeat; packed logical mips use 4/5=point clamp/repeat
    // and 6/7=linear clamp/repeat; 8..11 use those modes on physical mip chains
    // with a fractional logical crop. These floats are one shader attribute.
    float premultiplied{}, sampling{}, mipLevels{}, mipExtrusion{};
    float relativeX{}, relativeY{}, relativeZ{};
    // Coverage-relative source-pixel inset and extent. Separate floats avoid
    // subtracting nearly equal Atlas UV bounds for crops smaller than a texel.
    float logicalX{}, logicalY{}, logicalWidth{}, logicalHeight{};
};
void Validate(const Image&, Paint);
std::array<std::array<double,2>,4> ImageCorners(const Image&, Transform);
}
}
