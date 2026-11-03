#include "MipChain.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace core::modules::Graphic::Image::MipChain {
namespace {
using U = std::uint64_t;
constexpr std::uint32_t MaxDimension = 16384;
U Right(Atlas::Rect r) { return U(r.x) + r.width; }
U Bottom(Atlas::Rect r) { return U(r.y) + r.height; }
bool Inside(Atlas::Rect r, std::uint32_t width, std::uint32_t height) {
    return r.width && r.height && Right(r) <= width && Bottom(r) <= height;
}
void Cancel(Atlas::CancelFn cancel, void* data) {
    if(cancel && cancel(data)) throw Atlas::Cancelled();
}
std::size_t Bytes(std::uint32_t width, std::uint32_t height) {
    if(!width || !height || U(width) * height > std::numeric_limits<std::size_t>::max() / 4)
        throw std::invalid_argument("invalid mip page RGBA dimensions");
    return static_cast<std::size_t>(U(width) * height * 4);
}
void Source(Atlas::RgbaView source) {
    const auto row = Bytes(source.width, 1);
    if(!source.height || source.stride < row ||
       (source.height > 1 && source.stride > (std::numeric_limits<std::size_t>::max() - row) / (source.height - 1)) ||
       source.bytes.size() < static_cast<std::size_t>(source.height - 1) * source.stride + row)
        throw std::invalid_argument("invalid mip source RGBA span/stride");
}
Atlas::Rect Absolute(Atlas::Rect region, Atlas::Rect allocation) {
    // Layout/whole allocation validation already proves these additions fit.
    region.x += allocation.x; region.y += allocation.y;
    return region;
}
std::uint8_t* Pixel(std::span<std::uint8_t> page, std::size_t pitch, std::uint32_t x, std::uint32_t y) {
    return page.data() + static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * 4;
}
void CopyRow(std::uint8_t* target, const std::uint8_t* input, std::size_t bytes, Atlas::CancelFn cancel, void* data) {
    constexpr std::size_t Chunk = 64 * 1024;
    while(bytes) {
        Cancel(cancel, data);
        const auto count = std::min(bytes, Chunk);
        std::memcpy(target, input, count);
        target += count; input += count; bytes -= count;
    }
}
void Extrude(std::span<std::uint8_t> page, std::size_t pitch, Atlas::Rect region,
    std::uint32_t extrusion, Atlas::CancelFn cancel, void* data) {
    if(!extrusion) return;
    for(std::uint32_t y = 0; y < region.height; ++y) {
        Cancel(cancel, data);
        auto* content = Pixel(page, pitch, region.x, region.y + y);
        const auto* last = content + static_cast<std::size_t>(region.width - 1) * 4;
        auto* left = content - static_cast<std::size_t>(extrusion) * 4;
        auto* right = content + static_cast<std::size_t>(region.width) * 4;
        for(std::uint32_t x = 0; x < extrusion; ++x) {
            if(!(x & 255)) Cancel(cancel, data);
            std::memcpy(left + static_cast<std::size_t>(x) * 4, content, 4);
            std::memcpy(right + static_cast<std::size_t>(x) * 4, last, 4);
        }
    }
    const auto first = Pixel(page, pitch, region.x - extrusion, region.y);
    const auto last = Pixel(page, pitch, region.x - extrusion, region.y + region.height - 1);
    const auto width = static_cast<std::size_t>(U(region.width) + U(extrusion) * 2) * 4;
    for(std::uint32_t y = 0; y < extrusion; ++y) {
        CopyRow(Pixel(page, pitch, region.x - extrusion, region.y - extrusion + y), first, width, cancel, data);
        CopyRow(Pixel(page, pitch, region.x - extrusion, region.y + region.height + y), last, width, cancel, data);
    }
}
void Downsample(std::span<std::uint8_t> page, std::size_t pitch, Atlas::Rect previous,
    Atlas::Rect next, Atlas::CancelFn cancel, void* data) {
    // SDL generates hardware mips by a linear blit. Map destination pixel
    // centers to source texel coordinates: (x+0.5)*sw/dw-0.5. Integer fractions
    // preserve that bilinear kernel without platform-dependent floating ties.
    // Unlike an area filter, an odd 3->1 reduction selects the center texel.
    const U dx = U(next.width) * 2, dy = U(next.height) * 2;
    const U denominator = dx * dy;
    for(std::uint32_t y = 0; y < next.height; ++y) {
        Cancel(cancel, data);
        // Each next dimension is <= its predecessor, so these numerators are
        // nonnegative, including a one-pixel axis that remains one pixel.
        const U py = (U(y) * 2 + 1) * previous.height - next.height;
        const auto y0 = std::min(static_cast<std::uint32_t>(py / dy), previous.height - 1);
        const auto y1 = std::min(y0 + 1, previous.height - 1);
        const U fy = py % dy;
        for(std::uint32_t x = 0; x < next.width; ++x) {
            if(!(x & 255)) Cancel(cancel, data);
            const U px = (U(x) * 2 + 1) * previous.width - next.width;
            const auto x0 = std::min(static_cast<std::uint32_t>(px / dx), previous.width - 1);
            const auto x1 = std::min(x0 + 1, previous.width - 1);
            const U fx = px % dx;
            const auto* a = Pixel(page, pitch, previous.x + x0, previous.y + y0);
            const auto* b = Pixel(page, pitch, previous.x + x1, previous.y + y0);
            const auto* c = Pixel(page, pitch, previous.x + x0, previous.y + y1);
            const auto* d = Pixel(page, pitch, previous.x + x1, previous.y + y1);
            auto* output = Pixel(page, pitch, next.x + x, next.y + y);
            for(std::size_t channel = 0; channel < 4; ++channel) {
                const U sum = U(a[channel]) * (dx - fx) * (dy - fy) + U(b[channel]) * fx * (dy - fy) +
                    U(c[channel]) * (dx - fx) * fy + U(d[channel]) * fx * fy;
                output[channel] = static_cast<std::uint8_t>((sum + denominator / 2) / denominator);
            }
        }
    }
}
}

Layout Describe(std::uint32_t width, std::uint32_t height, std::uint32_t extrusion) {
    if(!width || !height || width > MaxDimension || height > MaxDimension)
        throw std::invalid_argument("mip Image dimensions must be within [1,16384]");
    Layout result;
    result.extrusion = extrusion;
    const U stripWidth = U(width) + U(extrusion) * 2;
    if(stripWidth > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("mip strip width overflow");
    result.width = static_cast<std::uint32_t>(stripWidth);
    U bottom{};
    for(;;) {
        const U nextBottom = bottom + height + U(extrusion) * 2;
        if(nextBottom > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("mip strip height overflow");
        result.regions[result.levels++] = {extrusion, static_cast<std::uint32_t>(bottom + extrusion), width, height};
        bottom = nextBottom;
        if(width == 1 && height == 1) break;
        width = std::max(1u, width / 2); height = std::max(1u, height / 2);
    }
    result.height = static_cast<std::uint32_t>(bottom);
    return result;
}

void Copy(std::span<std::uint8_t> destination, std::uint32_t pageWidth,
    std::uint32_t pageHeight, Atlas::Rect allocation, Atlas::RgbaView source,
    Atlas::Rect sourceRect, const Layout& layout, Atlas::CancelFn cancel, void* data) {
    const auto required = Bytes(pageWidth, pageHeight);
    if(destination.size() < required) throw std::invalid_argument("mip destination is too short");
    Source(source);
    if(!Inside(sourceRect, source.width, source.height)) throw std::invalid_argument("invalid mip source region");
    // Canonical validation also proves distinct content/guard footprints do
    // not overlap. It rejects forged dimensions, regions and unused entries.
    if(layout != Describe(sourceRect.width, sourceRect.height, layout.extrusion))
        throw std::invalid_argument("mip layout does not match the source region");
    if(allocation.width != layout.width || allocation.height != layout.height || !Inside(allocation, pageWidth, pageHeight))
        throw std::invalid_argument("invalid mip strip allocation");
    const auto from = reinterpret_cast<std::uintptr_t>(source.bytes.data());
    const auto to = reinterpret_cast<std::uintptr_t>(destination.data());
    if((from <= to && to - from < source.bytes.size()) || (to < from && from - to < required))
        throw std::invalid_argument("mip source and destination overlap");
    const auto pitch = static_cast<std::size_t>(pageWidth) * 4;
    Cancel(cancel, data);
    const auto base = Absolute(layout.regions[0], allocation);
    for(std::uint32_t y = 0; y < base.height; ++y) {
        Cancel(cancel, data);
        const auto* input = source.bytes.data() + static_cast<std::size_t>(sourceRect.y + y) * source.stride +
            static_cast<std::size_t>(sourceRect.x) * 4;
        std::memcpy(Pixel(destination, pitch, base.x, base.y + y), input, static_cast<std::size_t>(base.width) * 4);
    }
    Extrude(destination, pitch, base, layout.extrusion, cancel, data);
    for(std::uint32_t level = 1; level < layout.levels; ++level) {
        const auto previous = Absolute(layout.regions[level - 1], allocation);
        const auto next = Absolute(layout.regions[level], allocation);
        Downsample(destination, pitch, previous, next, cancel, data);
        Extrude(destination, pitch, next, layout.extrusion, cancel, data);
    }
    Cancel(cancel, data);
}
}
