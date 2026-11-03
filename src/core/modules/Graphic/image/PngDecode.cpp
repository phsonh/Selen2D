#include "PngDecode.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace {
namespace I = core::modules::Graphic::Image;

// Headers carry an identity, never a pointer into a completed decode's stack.
// Retained output may be destroyed on another thread or inside a later decode.
struct alignas(std::max_align_t) Allocation {
    std::uint64_t task{};
    std::size_t bytes{};
};
struct DecodeContext {
    std::uint64_t task{};
    std::size_t limit{}, resident{}, peak{};
    I::CancelFn cancel{};
    void* cancelData{};
    bool cancelled{}, exhausted{};

    bool IsCancelled() noexcept {
        if (!cancelled && cancel) cancelled = cancel(cancelData);
        return cancelled;
    }
};
thread_local DecodeContext* currentDecode{};
std::atomic<std::uint64_t> nextTask{1};

std::uint64_t NewTask() {
    auto next = nextTask.load(std::memory_order_relaxed);
    for (;;) {
        if (next == std::numeric_limits<std::uint64_t>::max())
            throw I::PngError(I::PngFailure::MemoryLimit, "PNG decode task identity exhausted");
        if (nextTask.compare_exchange_weak(next, next + 1, std::memory_order_relaxed)) return next;
    }
}
struct DecodeScope {
    explicit DecodeScope(DecodeContext& context) noexcept : previous(currentDecode) { currentDecode = &context; }
    ~DecodeScope() { currentDecode = previous; }
    DecodeContext* previous;
};

void* Allocate(std::size_t size) noexcept {
    auto* context = currentDecode;
    if (!context || context->IsCancelled()) return nullptr;
    // Charge the real allocation including our header; even a zero-size request
    // owns one payload byte. The checked subtraction also prevents overflow.
    const auto payload = std::max<std::size_t>(size, 1);
    if (payload > std::numeric_limits<std::size_t>::max() - sizeof(Allocation)) {
        context->exhausted = true;
        return nullptr;
    }
    const auto total = payload + sizeof(Allocation);
    if (context->resident > context->limit || total > context->limit - context->resident) {
        context->exhausted = true;
        return nullptr;
    }
    auto* block = static_cast<Allocation*>(std::malloc(total));
    if (!block) {
        context->exhausted = true;
        return nullptr;
    }
    block->task = context->task;
    block->bytes = total;
    context->resident += total;
    context->peak = std::max(context->peak, context->resident);
    return block + 1;
}
void Free(void* pixels) noexcept {
    if (!pixels) return;
    auto* block = static_cast<Allocation*>(pixels) - 1;
    if (currentDecode && currentDecode->task == block->task)
        currentDecode->resident -= block->bytes;
    std::free(block);
}
void* Reallocate(void* pixels, std::size_t, std::size_t size) noexcept {
    if (!pixels) return Allocate(size);
    auto* block = static_cast<Allocation*>(pixels) - 1;
    // Deliberately allocate before freeing. realloc's internal transient peak is
    // otherwise invisible, so a strict byte limit could be exceeded unnoticed.
    void* replacement = Allocate(size);
    if (!replacement) return nullptr;
    std::memcpy(replacement, pixels, std::min(size, block->bytes - sizeof(Allocation)));
    Free(pixels);
    return replacement;
}
}

// Compile pinned stb_image with the documented palette-index hardening once,
// with only PNG enabled (see 3rd_party/stb_image/README.md).
// No decoder allocation bypasses the task allocator. Disabling failure strings
// also removes stb's mutable static unknown-chunk diagnostic buffer.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_FAILURE_STRINGS
#define STBI_MALLOC(size) Allocate(size)
#define STBI_REALLOC_SIZED(p, oldSize, newSize) Reallocate(p, oldSize, newSize)
#define STBI_FREE(p) Free(p)
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "stb_image.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace {
constexpr std::array<std::uint8_t, 8> pngSignature{137, 80, 78, 71, 13, 10, 26, 10};
std::uint32_t Read32(const std::uint8_t* bytes) noexcept {
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16)
        | (std::uint32_t(bytes[2]) << 8) | bytes[3];
}
std::uint32_t Crc(std::span<const std::uint8_t> bytes, DecodeContext* context = nullptr) {
    std::uint32_t crc = 0xffffffffu;
    std::size_t checked{};
    for (const auto value : bytes) {
        if (context && (checked++ & 4095u) == 0 && context->IsCancelled())
            throw I::PngError(I::PngFailure::Cancelled, "PNG decoding cancelled");
        crc ^= value;
        for (unsigned bit{}; bit != 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return crc ^ 0xffffffffu;
}
bool Type(const std::uint8_t* bytes, const char* name) noexcept { return std::memcmp(bytes, name, 4) == 0; }

void ValidateChunks(std::span<const std::uint8_t> bytes, DecodeContext& context) {
    bool data{}, finishedData{}, palette{};
    std::size_t offset = 8;
    while (offset < bytes.size()) {
        if (context.IsCancelled()) throw I::PngError(I::PngFailure::Cancelled, "PNG decoding cancelled");
        if (bytes.size() - offset < 12)
            throw I::PngError(I::PngFailure::Invalid, "PNG chunk is truncated");
        const auto length = Read32(bytes.data() + offset);
        if (length > std::uint32_t(std::numeric_limits<int>::max()) || length > bytes.size() - offset - 12)
            throw I::PngError(I::PngFailure::Invalid, "PNG chunk length is invalid or truncated");
        const auto* type = bytes.data() + offset + 4;
        for (unsigned index{}; index != 4; ++index)
            if (!((type[index] >= 'A' && type[index] <= 'Z') || (type[index] >= 'a' && type[index] <= 'z')))
                throw I::PngError(I::PngFailure::Invalid, "PNG chunk type is invalid");
        if ((type[2] & 32u) != 0)
            throw I::PngError(I::PngFailure::Invalid, "PNG chunk uses a reserved type bit");
        // In particular, do not silently accept Apple's nonstandard CgBI/BGRA
        // path and then label its pixels as straight RGBA.
        if ((type[0] & 32u) == 0 && !Type(type, "IHDR") && !Type(type, "PLTE")
            && !Type(type, "IDAT") && !Type(type, "IEND"))
            throw I::PngError(I::PngFailure::Invalid, "PNG contains an unsupported critical chunk");
        if (Read32(type + 4 + length) != Crc(bytes.subspan(offset + 4, std::size_t(length) + 4), &context))
            throw I::PngError(I::PngFailure::Invalid, "PNG chunk CRC does not match");
        if (Type(type, "IHDR") && offset != 8)
            throw I::PngError(I::PngFailure::Invalid, "PNG contains more than one IHDR");
        if (Type(type, "IDAT")) {
            if (finishedData) throw I::PngError(I::PngFailure::Invalid, "PNG IDAT chunks are not consecutive");
            data = true;
        } else if (data) {
            finishedData = true;
        }
        if (Type(type, "PLTE")) {
            if (palette || data || length == 0 || length > 768 || length % 3 != 0
                || bytes[25] == 0 || bytes[25] == 4
                || (bytes[25] == 3 && length / 3 > (1u << bytes[24])))
                throw I::PngError(I::PngFailure::Invalid, "PNG palette is invalid");
            palette = true;
        }
        if (Type(type, "IEND")) {
            if (!data || length != 0 || offset + 12 != bytes.size())
                throw I::PngError(I::PngFailure::Invalid, "PNG ending is invalid");
            if (bytes[25] == 3 && !palette)
                throw I::PngError(I::PngFailure::Invalid, "PNG indexed image has no palette");
            return;
        }
        offset += std::size_t(length) + 12;
    }
    throw I::PngError(I::PngFailure::Invalid, "PNG has no IEND chunk");
}

struct Input {
    std::span<const std::uint8_t> bytes;
    std::size_t offset{};
    DecodeContext& context;
};
int Read(void* user, char* target, int size) noexcept {
    auto& input = *static_cast<Input*>(user);
    if (size <= 0 || input.context.IsCancelled()) return 0;
    const auto available = std::min<std::size_t>(static_cast<std::size_t>(size), input.bytes.size() - input.offset);
    std::memcpy(target, input.bytes.data() + input.offset, available);
    input.offset += available;
    return static_cast<int>(available);
}
void Skip(void* user, int size) noexcept {
    auto& input = *static_cast<Input*>(user);
    if (input.context.IsCancelled()) { input.offset = input.bytes.size(); return; }
    if (size < 0) {
        const auto backward = static_cast<std::size_t>(-static_cast<std::int64_t>(size));
        input.offset -= std::min(backward, input.offset);
    } else {
        input.offset += std::min<std::size_t>(static_cast<std::size_t>(size), input.bytes.size() - input.offset);
    }
}
int Eof(void* user) noexcept {
    auto& input = *static_cast<Input*>(user);
    return input.context.IsCancelled() || input.offset == input.bytes.size();
}
}

namespace core::modules::Graphic::Image {
void FreePixels::operator()(std::uint8_t* pixels) const noexcept { Free(pixels); }

Dimensions InspectPng(std::span<const std::uint8_t> prefix, std::uint32_t maxDimension) {
    if (prefix.size() < 33 || !std::equal(pngSignature.begin(), pngSignature.end(), prefix.begin())
        || Read32(prefix.data() + 8) != 13 || !Type(prefix.data() + 12, "IHDR"))
        throw PngError(PngFailure::Invalid, "PNG signature or IHDR is invalid or truncated");
    const auto width = Read32(prefix.data() + 16), height = Read32(prefix.data() + 20);
    if (!width || !height || !maxDimension)
        throw PngError(PngFailure::Invalid, "PNG dimensions and dimension limit must be positive");
    if (width > maxDimension || height > maxDimension || width > (1u << 24) || height > (1u << 24)
        || width > static_cast<std::size_t>(std::numeric_limits<int>::max()) / 4 / height
        || width > std::numeric_limits<std::size_t>::max() / 4 / height)
        throw PngError(PngFailure::TooLarge, "PNG dimensions exceed the supported limit");
    const auto depth = prefix[24], color = prefix[25];
    const bool validDepth = (color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16))
        || (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8))
        || ((color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16));
    if (!validDepth || prefix[26] != 0 || prefix[27] != 0 || prefix[28] > 1)
        throw PngError(PngFailure::Invalid, "PNG IHDR color, depth or compression method is invalid");
    if (Read32(prefix.data() + 29) != Crc(prefix.subspan(12, 17)))
        throw PngError(PngFailure::Invalid, "PNG IHDR CRC does not match");
    return {width, height};
}

PngImage DecodePng(std::span<const std::uint8_t> bytes, std::uint32_t maxDimension,
    std::size_t allocationLimit, CancelFn cancel, void* cancelData) {
    DecodeContext context{NewTask(), allocationLimit, 0, 0, cancel, cancelData};
    DecodeScope scope(context);
    if (context.IsCancelled()) throw PngError(PngFailure::Cancelled, "PNG decoding cancelled");
    const auto dimensions = InspectPng(bytes, maxDimension);
    ValidateChunks(bytes, context);
    Input input{bytes, 0, context};
    const stbi_io_callbacks callbacks{Read, Skip, Eof};
    int width{}, height{}, channels{};
    std::unique_ptr<std::uint8_t, FreePixels> pixels(
        stbi_load_from_callbacks(&callbacks, &input, &width, &height, &channels, 4));
    if (context.IsCancelled()) throw PngError(PngFailure::Cancelled, "PNG decoding cancelled");
    if (context.exhausted) {
        const std::string message = "PNG decode allocation budget exhausted (peak=" + std::to_string(context.peak)
            + ", limit=" + std::to_string(context.limit) + ")";
        throw PngError(PngFailure::MemoryLimit, message.c_str());
    }
    if (!pixels || width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) != dimensions.width
        || static_cast<std::uint32_t>(height) != dimensions.height)
        throw PngError(PngFailure::Invalid, "PNG pixel stream is invalid or truncated");
    return {dimensions.width, dimensions.height, std::move(pixels), context.resident, context.peak};
}
}
