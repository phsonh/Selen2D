#pragma once
#include "Atlas.h"
#include <array>
#include <cstdint>
#include <span>

// CPU-only Image-local mip strip. The caller owns source/page byte admission;
// this module does not allocate pixels or depend on Resource, SDL or Lua.
namespace core::modules::Graphic::Image::MipChain {
struct Layout {
    std::uint32_t width{}, height{}, levels{}, extrusion{};
    // Content rectangles relative to the strip's top-left, not allocation
    // rectangles. Unused entries are zero. No rotation or logical-image scale.
    std::array<Atlas::Rect, 15> regions{};
    bool operator==(const Layout&) const = default;
};
// Source dimensions must be in [1,16384]. Each level is floor-halved, clamped
// to one pixel per axis; all levels have their own RGBA edge extrusion.
Layout Describe(std::uint32_t width, std::uint32_t height, std::uint32_t extrusion = 2);

// Writes only the level contents/guards inside allocation. Unused space to the
// right of smaller levels remains untouched. Source must not alias the page.
// Each lower-level pixel samples the preceding level at its mapped pixel center
// using bilinear filtering and edge clamp, matching SDL's linear mip blit kernel.
// RGBA channels are independent (no gamma/PMA conversion); UNORM8 conversion
// rounds nearest, ties up. Hardware UNORM rounding can differ by one unit.
// Cancellation throws Atlas::Cancelled and may leave partial unpublished data.
void Copy(std::span<std::uint8_t> destination, std::uint32_t pageWidth,
    std::uint32_t pageHeight, Atlas::Rect allocation, Atlas::RgbaView source,
    Atlas::Rect sourceRect, const Layout&, Atlas::CancelFn = nullptr, void* = nullptr);
}
