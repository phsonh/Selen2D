#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

namespace core::modules::Graphic::Image {
using CancelFn = bool (*)(void*) noexcept;
struct Dimensions { std::uint32_t width{}, height{}; };
enum class PngFailure { Invalid, TooLarge, MemoryLimit, Cancelled };
class PngError final : public std::runtime_error {
public:
    PngError(PngFailure code_, const char* message) : std::runtime_error(message), code(code_) {}
    PngFailure code;
};
struct FreePixels { void operator()(std::uint8_t*) const noexcept; };
struct PngImage {
    std::uint32_t width{}, height{};
    std::unique_ptr<std::uint8_t, FreePixels> pixels;
    // Actual custom allocator bytes, including allocation headers. Source bytes
    // are borrowed and separately accounted by the preparation layer.
    std::size_t residentBytes{}, peakBytes{};
    std::span<const std::uint8_t> Bytes() const noexcept {
        return {pixels.get(), static_cast<std::size_t>(width) * height * 4};
    }
};
// PNG only, top-left origin, straight RGBA8; preserves transparent RGB. No
// premultiplication, implicit scale, Y flip, gamma or Object collision data.
Dimensions InspectPng(std::span<const std::uint8_t> prefix, std::uint32_t maxDimension);
PngImage DecodePng(std::span<const std::uint8_t> bytes, std::uint32_t maxDimension,
    std::size_t allocationLimit, CancelFn = nullptr, void* cancelData = nullptr);
}
