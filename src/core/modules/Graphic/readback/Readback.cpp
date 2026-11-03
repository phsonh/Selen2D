#include "Readback.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace core::modules::Graphic::Readback {
namespace {
std::runtime_error Error(const char* operation) { return std::runtime_error(std::string(operation) + ": " + SDL_GetError()); }
// All callbacks contain exceptions. They are called through SDL's C ABI and
// must never unwind through the PNG encoder. Failed writes are all-or-nothing:
// SDL's current PNG encoder treats any nonzero WriteIO result as success.
struct Output {
    std::size_t limit{}, position{};
    std::string bytes;
    bool failed{};
    void Fail(const char* message, SDL_IOStatus* status = nullptr) noexcept {
        failed = true;
        if (status) *status = SDL_IO_STATUS_ERROR;
        SDL_SetError("PNG output: %s", message);
    }
    static Sint64 SDLCALL Size(void* context) noexcept { return static_cast<Sint64>(static_cast<Output*>(context)->bytes.size()); }
    static Sint64 SDLCALL Seek(void* context, Sint64 offset, SDL_IOWhence whence) noexcept {
        auto& out = *static_cast<Output*>(context);
        std::uint64_t base{};
        if (whence == SDL_IO_SEEK_CUR) base = out.position;
        else if (whence == SDL_IO_SEEK_END) base = out.bytes.size();
        else if (whence != SDL_IO_SEEK_SET) { out.Fail("invalid seek origin"); return -1; }
        if (out.failed) return -1;
        if (offset < 0) {
            const auto backward = static_cast<std::uint64_t>(-(offset + 1)) + 1;
            if (backward > base) { out.Fail("seek before beginning"); return -1; }
            base -= backward;
        } else {
            if (static_cast<std::uint64_t>(offset) > out.limit - base) { out.Fail("seek exceeds byte budget"); return -1; }
            base += static_cast<std::uint64_t>(offset);
        }
        out.position = static_cast<std::size_t>(base); return static_cast<Sint64>(base);
    }
    static std::size_t SDLCALL Write(void* context, const void* data, std::size_t size, SDL_IOStatus* status) noexcept {
        auto& out = *static_cast<Output*>(context);
        if (out.failed) { *status = SDL_IO_STATUS_ERROR; return 0; }
        if (size > out.limit - out.position) { out.Fail("encoded PNG exceeds byte budget", status); return 0; }
        try {
            const auto end = out.position + size;
            if (end > out.bytes.size()) out.bytes.resize(end);
            std::memcpy(out.bytes.data() + out.position, data, size); out.position = end; return size;
        } catch (const std::exception& error) { out.Fail(error.what(), status); }
        catch (...) { out.Fail("allocation failed", status); }
        return 0;
    }
    static bool SDLCALL Flush(void* context, SDL_IOStatus* status) noexcept {
        const auto& out = *static_cast<Output*>(context);
        if (out.failed) *status = SDL_IO_STATUS_ERROR;
        return !out.failed;
    }
    static bool SDLCALL Close(void* context) noexcept { return !static_cast<Output*>(context)->failed; }
};
std::uint8_t Straight(std::uint8_t color, std::uint8_t alpha) {
    return alpha ? static_cast<std::uint8_t>(std::min(255u, (unsigned(color) * 255u + alpha / 2u) / alpha)) : std::uint8_t{0};
}
}
struct Request::Impl {
    SDL_GPUDevice* device;
    const std::thread::id owner{std::this_thread::get_id()};
    std::uint32_t width{}, height{}, pitch{}, tightPitch{}, transferBytes{}, pixelBytes{};
    SDL_GPUTransferBuffer* transfer{};
    Config config;
    bool bgra{}, window{}, encoded{};
    Impl(SDL_GPUDevice* d, std::uint32_t w, std::uint32_t h, SDL_GPUTextureFormat format, bool win, Config c)
        : device(d), width(w), height(h), config(c), bgra(format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM), window(win) {
        if (!d || !w || !h || w > 16384 || h > 16384)
            throw std::invalid_argument("PNG readback requires a device and dimensions in 1..16384");
        if (format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM && format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)
            throw std::invalid_argument("PNG readback requires resolved RGBA8 or BGRA8 UNORM input");
        if (!c.maxBytes || !c.maxPngBytes || c.maxPngBytes > std::string{}.max_size() ||
            c.maxPngBytes > static_cast<std::uint64_t>(std::numeric_limits<Sint64>::max()))
            throw std::invalid_argument("PNG readback requires bounded nonzero storage/output limits");
        const std::uint64_t row = std::uint64_t(w) * 4;
        const std::uint64_t aligned = (row + 255) & ~std::uint64_t(255);
        const std::uint64_t total = aligned * h, pixels = row * h;
        if (total > std::numeric_limits<Uint32>::max() || pixels > std::numeric_limits<std::size_t>::max() ||
            row > std::numeric_limits<int>::max() || total > c.maxBytes || pixels > c.maxBytes - total)
            throw std::length_error("PNG readback transfer and pixel storage exceed byte budget");
        pitch = static_cast<std::uint32_t>(aligned); tightPitch = static_cast<std::uint32_t>(row);
        transferBytes = static_cast<std::uint32_t>(total); pixelBytes = static_cast<std::uint32_t>(pixels);
        SDL_GPUTransferBufferCreateInfo create{}; create.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; create.size = transferBytes;
        transfer = SDL_CreateGPUTransferBuffer(device, &create);
        if (!transfer) throw Error("Create PNG readback transfer");
    }
    ~Impl() {
        if (owner != std::this_thread::get_id()) std::terminate();
        if (transfer) SDL_ReleaseGPUTransferBuffer(device, transfer);
    }
    void Thread() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("PNG readback requires the render owner thread");
    }
};
Request::Request(SDL_GPUDevice* d, std::uint32_t w, std::uint32_t h, SDL_GPUTextureFormat format, bool window, Config c)
    : impl_(std::make_unique<Impl>(d, w, h, format, window, c)) {}
Request::~Request() = default;
void Request::Encode(SDL_GPUCommandBuffer* command, SDL_GPUTexture* source) {
    impl_->Thread();
    if (!command || !source) throw std::invalid_argument("PNG readback requires a command buffer and source texture");
    if (impl_->encoded) throw std::logic_error("PNG readback request was already encoded");
    auto* pass = SDL_BeginGPUCopyPass(command);
    if (!pass) throw Error("Begin PNG readback copy pass");
    struct EndPass { SDL_GPUCopyPass* value; ~EndPass() { if (value) SDL_EndGPUCopyPass(value); } } guard{pass};
    SDL_GPUTextureRegion from{}; from.texture = source; from.w = impl_->width; from.h = impl_->height; from.d = 1;
    SDL_GPUTextureTransferInfo to{};
    to.transfer_buffer = impl_->transfer; to.pixels_per_row = impl_->pitch / 4; to.rows_per_layer = impl_->height;
    SDL_ClearError(); SDL_DownloadFromGPUTexture(pass, &from, &to);
    if (*SDL_GetError()) throw Error("Encode PNG readback download");
    SDL_EndGPUCopyPass(pass); guard.value = nullptr;
    if (*SDL_GetError()) throw Error("End PNG readback copy pass");
    impl_->encoded = true;
}
std::string Request::PNG() {
    impl_->Thread();
    if (!impl_->encoded) throw std::logic_error("PNG readback must be encoded and its fence completed before PNG()");
    std::vector<std::uint8_t> pixels(impl_->pixelBytes);
    {
        const auto* source = static_cast<const std::uint8_t*>(SDL_MapGPUTransferBuffer(impl_->device, impl_->transfer, false));
        if (!source) throw Error("Map completed PNG readback transfer");
        struct Unmap { SDL_GPUDevice* device; SDL_GPUTransferBuffer* transfer;
            ~Unmap() { SDL_UnmapGPUTransferBuffer(device, transfer); } } unmap{impl_->device, impl_->transfer};
        for (std::uint32_t y = 0; y < impl_->height; ++y) for (std::uint32_t x = 0; x < impl_->width; ++x) {
            const auto* input = source + std::size_t(y) * impl_->pitch + x * 4;
            auto* output = pixels.data() + std::size_t(y) * impl_->tightPitch + x * 4;
            const auto alpha = input[3];
            const std::uint8_t r = input[impl_->bgra ? 2 : 0], g = input[1], b = input[impl_->bgra ? 0 : 2];
            output[0] = impl_->window ? r : Straight(r, alpha);
            output[1] = impl_->window ? g : Straight(g, alpha);
            output[2] = impl_->window ? b : Straight(b, alpha);
            output[3] = impl_->window ? std::uint8_t{255} : alpha;
        }
    }
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(SDL_CreateSurfaceFrom(
        static_cast<int>(impl_->width), static_cast<int>(impl_->height), SDL_PIXELFORMAT_RGBA32,
        pixels.data(), static_cast<int>(impl_->tightPitch)), SDL_DestroySurface);
    if (!surface) throw Error("Create PNG readback surface");
    // Declares SDR interpretation, without applying an extra gamma conversion.
    if (!SDL_SetSurfaceColorspace(surface.get(), SDL_COLORSPACE_SRGB)) throw Error("Set PNG readback colorspace");
    Output output; output.limit = static_cast<std::size_t>(impl_->config.maxPngBytes);
    SDL_IOStreamInterface callbacks{}; SDL_INIT_INTERFACE(&callbacks);
    callbacks.size = Output::Size; callbacks.seek = Output::Seek; callbacks.write = Output::Write;
    callbacks.flush = Output::Flush; callbacks.close = Output::Close;
    std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)> stream(SDL_OpenIO(&callbacks, &output), SDL_CloseIO);
    if (!stream) throw Error("Create bounded PNG output");
    if (!SDL_SavePNG_IO(surface.get(), stream.get(), false) || output.failed || output.bytes.empty()) throw Error("Encode PNG bytes");
    if (!SDL_CloseIO(stream.release())) throw Error("Close PNG output");
    return std::move(output.bytes);
}
}
