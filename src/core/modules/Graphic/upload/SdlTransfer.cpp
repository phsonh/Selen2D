#include "Upload.h"
#include <SDL3/SDL.h>
#include <limits>
#include <stdexcept>
#include <utility>

namespace core::modules::Graphic::Upload {
namespace {
// Borrowed SDL_GPU device. No native graphics API, window ownership, Object
// traversal, resource registry, or independent upload thread lives here.
class SdlDevice final : public TransferDevice {
public:
    explicit SdlDevice(SDL_GPUDevice* device_) : device(device_) {
        if (!device) throw std::invalid_argument("SDL upload device must not be null");
        error.reserve(256);
    }

    SDL_GPUTexture* CreateTexture(std::uint32_t width, std::uint32_t height,
        std::uint32_t levels) override {
        error.clear();
        std::uint32_t maximumLevels = 1;
        for (auto dimension = width > height ? width : height; dimension > 1; dimension >>= 1)
            ++maximumLevels;
        if (!width || !height || !levels || levels > maximumLevels) {
            error = "CreateGPUTexture: invalid RGBA8 dimensions or mip level count";
            return nullptr;
        }
        const SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
            (levels > 1 ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
        if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                SDL_GPU_TEXTURETYPE_2D, usage)) {
            error = "CreateGPUTexture: device does not support RGBA8 UNORM 2D with requested sampler/mipmap usage";
            return nullptr;
        }
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = usage;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = levels;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        auto* result = SDL_CreateGPUTexture(device, &info);
        if (!result) CaptureError("SDL_CreateGPUTexture");
        return result;
    }

    SDL_GPUTransferBuffer* CreateTransfer(std::uint32_t bytes) override {
        error.clear();
        if (!bytes) {
            error = "CreateGPUTransferBuffer: byte size must be positive";
            return nullptr;
        }
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = bytes;
        auto* result = SDL_CreateGPUTransferBuffer(device, &info);
        if (!result) CaptureError("SDL_CreateGPUTransferBuffer");
        return result;
    }

    void* Map(SDL_GPUTransferBuffer* transfer) override {
        error.clear();
        if (!transfer) {
            error = "SDL_MapGPUTransferBuffer: null transfer buffer";
            return nullptr;
        }
        // Queue reuses a bounded slot only after its fence has signalled. Never
        // ask SDL to cycle and silently allocate extra in-flight backing stores.
        auto* result = SDL_MapGPUTransferBuffer(device, transfer, false);
        if (!result) CaptureError("SDL_MapGPUTransferBuffer");
        return result;
    }

    void Unmap(SDL_GPUTransferBuffer* transfer) override {
        SDL_UnmapGPUTransferBuffer(device, transfer);
    }

    Submission Submit(SDL_GPUTransferBuffer* transfer, SDL_GPUTexture* texture,
        std::uint32_t y, std::uint32_t width, std::uint32_t rows,
        std::uint32_t rowPitch) override {
        error.clear();
        if (!transfer || !texture || !width || !rows || !rowPitch || rowPitch % 256 != 0 ||
            ((std::uint64_t(width) * 4 + 255) / 256) * 256 != rowPitch ||
            std::uint64_t(rowPitch) * rows > std::numeric_limits<std::uint32_t>::max() ||
            std::uint64_t(y) + rows > std::numeric_limits<std::uint32_t>::max()) {
            error = "SDL texture upload: invalid handles, dimensions, or 256-byte row pitch";
            return {};
        }

        auto* raw = SDL_AcquireGPUCommandBuffer(device);
        if (!raw) {
            CaptureError("SDL_AcquireGPUCommandBuffer");
            return {};
        }
        PendingCommand command(raw);
        SDL_ClearError();
        auto* pass = SDL_BeginGPUCopyPass(raw);
        if (!pass) {
            CaptureError("SDL_BeginGPUCopyPass");
            return CancelBeforeSubmit(command);
        }
        command.pass = pass;
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transfer;
        source.offset = 0; // Also satisfies conservative 512-byte placement alignment.
        source.pixels_per_row = rowPitch / 4;
        source.rows_per_layer = rows; // This stripe, not the full destination height.
        SDL_GPUTextureRegion destination{};
        destination.texture = texture;
        destination.mip_level = 0;
        destination.layer = 0;
        destination.x = 0;
        destination.y = y;
        destination.z = 0;
        destination.w = width;
        destination.h = rows;
        destination.d = 1;

        SDL_ClearError();
        // All stripes target one stable texture backing. Cycling a later stripe
        // would invalidate the previous stripes across the entire texture.
        SDL_UploadToGPUTexture(pass, &source, &destination, false);
        if (*SDL_GetError()) {
            // The public operation returns void; capture an SDL-reported backend
            // error before End/Cancel can replace the thread-local error string.
            CaptureError("SDL_UploadToGPUTexture");
            return CancelBeforeSubmit(command);
        }
        SDL_EndGPUCopyPass(pass);
        command.pass = nullptr;

        return SubmitRecorded(command);
    }

    Submission GenerateMipmaps(SDL_GPUTexture* texture) override {
        error.clear();
        if (!texture) {
            error = "SDL_GenerateMipmapsForGPUTexture: null texture";
            return {};
        }
        // Queue calls this only for textures created with levels > 1, after all
        // base-level stripes have completed. SDL owns the format-specific
        // generation algorithm and barriers; no native graphics API is used.
        auto* raw = SDL_AcquireGPUCommandBuffer(device);
        if (!raw) {
            CaptureError("SDL_AcquireGPUCommandBuffer for mipmaps");
            return {};
        }
        PendingCommand command(raw);
        SDL_ClearError();
        // This is deliberately OUTSIDE a copy/render/compute pass. SDL may
        // internally record blits or create a blit pipeline on first use.
        SDL_GenerateMipmapsForGPUTexture(raw, texture);
        if (*SDL_GetError()) {
            CaptureError("SDL_GenerateMipmapsForGPUTexture");
            return CancelBeforeSubmit(command);
        }
        // A completed base-upload fence is insufficient: this independently
        // consumed generation command buffer has its own completion fence.
        return SubmitRecorded(command);
    }

    bool Poll(SDL_GPUFence* fence) override {
        // False is normal nonblocking "not signalled", not a timeout/failure.
        return SDL_QueryGPUFence(device, fence);
    }

    void Release(SDL_GPUFence* fence) noexcept override { SDL_ReleaseGPUFence(device, fence); }
    void Release(SDL_GPUTexture* texture) noexcept override { SDL_ReleaseGPUTexture(device, texture); }
    void Release(SDL_GPUTransferBuffer* transfer) noexcept override { SDL_ReleaseGPUTransferBuffer(device, transfer); }

    bool WaitIdle() noexcept override {
        const auto success = SDL_WaitForGPUIdle(device);
        if (!success) {
            try { CaptureError("SDL_WaitForGPUIdle"); }
            catch (...) {
                // Reserved constructor storage makes this short fallback avoid
                // allocation; shutdown must not propagate a C++ exception.
                error = "SDL_WaitForGPUIdle failed";
            }
        }
        return success;
    }

    std::string Error() const override { return error; }

private:
    struct PendingCommand {
        SDL_GPUCommandBuffer* buffer{};
        SDL_GPUCopyPass* pass{};
        explicit PendingCommand(SDL_GPUCommandBuffer* value) : buffer(value) {}
        ~PendingCommand() {
            if (buffer) {
                if (pass) SDL_EndGPUCopyPass(pass);
                SDL_CancelGPUCommandBuffer(buffer);
            }
        }
        bool Cancel() noexcept {
            if (pass) { SDL_EndGPUCopyPass(pass); pass = nullptr; }
            return SDL_CancelGPUCommandBuffer(std::exchange(buffer, nullptr));
        }
    };
    Submission SubmitRecorded(PendingCommand& command) noexcept {
        // Submit consumes the command buffer even when it returns a null fence.
        // In particular a backend can execute it and only then fail to create or
        // signal its fence. Never cancel, retry, or reuse that CB after this call.
        auto* consumed = std::exchange(command.buffer, nullptr);
        auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(consumed);
        if (!fence) {
            CaptureError("SDL_SubmitGPUCommandBufferAndAcquireFence");
            return {nullptr, true};
        }
        return {fence, false};
    }
    Submission CancelBeforeSubmit(PendingCommand& command) noexcept {
        if (command.Cancel()) return {};
        // No swapchain was acquired. Still, a failed cancellation means there is
        // no proven terminal command state; Queue must quarantine this slot.
        try {
            const auto* cause = SDL_GetError();
            error += "; SDL_CancelGPUCommandBuffer failed";
            if (cause && *cause) { error += ": "; error += cause; }
        } catch (...) { error = "SDL command cancellation failed; command state is uncertain"; }
        return {nullptr, true};
    }
    void CaptureError(const char* operation) noexcept {
        try {
            const auto* cause = SDL_GetError();
            error = operation;
            error += ": ";
            error += cause && *cause ? cause : "SDL returned failure without an error message";
        } catch (...) {
            // Also protects post-submit uncertainty from being hidden by an
            // exception while formatting a long driver error. Storage >= 256
            // was reserved in the constructor and is never shrunk.
            error = "SDL GPU operation failed; diagnostic allocation failed";
        }
    }
    SDL_GPUDevice* device{};
    std::string error;
};
}

std::unique_ptr<TransferDevice> SdlTransferDevice(SDL_GPUDevice* device) {
    return std::make_unique<SdlDevice>(device);
}
}
