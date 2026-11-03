#pragma once
#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <memory>
#include <string>

namespace core::modules::Graphic::Readback {
struct Config {
    // Aligned GPU transfer + tightly packed CPU RGBA conversion storage.
    // SDL's encoder owns additional bounded-by-input compression workspace;
    // neither limit claims to cap process RSS or driver allocation overhead.
    std::uint64_t maxBytes{256ull * 1024 * 1024};
    std::uint64_t maxPngBytes{256ull * 1024 * 1024};
};
// One explicit download. Owner-thread only, including destruction. No Lua,
// File, frame scheduling, submission, fence wait or source ownership here.
class Request final {
public:
    Request(SDL_GPUDevice*, std::uint32_t width, std::uint32_t height,
        SDL_GPUTextureFormat, bool window, Config = {});
    ~Request();
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    // CB must belong to this device, have no active pass, and follow all source
    // writes. Source must match the constructor's dimensions/format. Call once.
    // Caller retains Request + source through fence completion (or CB cancel).
    void Encode(SDL_GPUCommandBuffer*, SDL_GPUTexture*);
    // Caller MUST wait for the encoded download's successful submission fence
    // before calling this. Repeated PNG calls are allowed. Returns independent
    // complete RGBA8 PNG bytes; has no disk side effects. Rows stay top-left.
    // Offscreen PMA is unassociated, A=0 -> RGB=0. Window RGB is displayed RGB
    // unchanged with A=255. SDR samples are interpreted as sRGB, no gamma pass.
    std::string PNG();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
