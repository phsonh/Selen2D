#pragma once
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <vector>

// CPU layout/rasterization only. No Resource registry, Object, Lua, SDL or GPU.
// Call on a CPU worker; caller owns admission and lifetime of source/page bytes.
namespace core::modules::Graphic::Image::Atlas {
using CancelFn = bool (*)(void*) noexcept;
class Cancelled final : public std::exception {
public:
    const char* what() const noexcept override { return "atlas CPU operation cancelled"; }
};
struct Rect {
    std::uint32_t x{}, y{}, width{}, height{};
    bool operator==(const Rect&) const = default;
};
struct Item { std::uint32_t width{}, height{}; };
struct Config {
    std::uint32_t targetSide{2048}, maxSide{4096}, extrusion{2};
    std::size_t maxItems{8192}, maxPages{128}, maxFreeRects{32768};
    std::uint64_t maxWork{64 * 1024 * 1024};
    std::uint64_t maxOutputBytes{256 * 1024 * 1024};
};
struct Placement {
    std::size_t page{};
    Rect allocation, content; // Top-left pixel origin; no rotation or trim.
    bool operator==(const Placement&) const = default;
};
struct Page {
    std::uint32_t width{}, height{};
    std::uint64_t contentPixels{}, allocatedPixels{};
    bool operator==(const Page&) const = default;
};
struct Plan {
    std::vector<Page> pages;
    std::vector<Placement> placements; // Same order as input; not packing order.
    std::uint32_t extrusion{};
    bool operator==(const Plan&) const = default;
};
// Only compatible atlas images here. Over-limit images are rejected, not resized
// or silently changed to standalone. Resource backing selection is separate.
Plan Pack(std::span<const Item>, Config = {}, CancelFn = nullptr, void* = nullptr);

struct RgbaView {
    std::span<const std::uint8_t> bytes;
    std::uint32_t width{}, height{};
    std::size_t stride{}; // >= width*4; may contain sheet rows/gutters.
};
// Copy one effective source region and duplicate the nearest edge including
// all four corners/RGBA. No alpha zeroing, scaling, Y flip, or mip generation.
// Destination must be zeroed by caller before first placement. No allocation.
// A cancelled operation may leave partial bytes; discard that unpublished page.
void CopyExtruded(std::span<std::uint8_t> destination, std::uint32_t pageWidth,
    std::uint32_t pageHeight, const Placement&, RgbaView source, Rect sourceRect,
    std::uint32_t extrusion = 2, CancelFn = nullptr, void* = nullptr);
}
