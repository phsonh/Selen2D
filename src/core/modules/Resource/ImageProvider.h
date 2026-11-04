#pragma once
#include "Resource.h"
#include "ImageSources.h"
#include "core/modules/Graphic/image/Atlas.h"
#include "core/modules/Graphic/upload/Upload.h"

// Internal bridge. The registry remains GPU-independent; Graphic does not know
// resource names or Object Pool. This is not a public Lua Texture interface.
namespace core::modules::Resource::Images {
namespace Atlas = core::modules::Graphic::Image::Atlas;
namespace Upload = core::modules::Graphic::Upload;
namespace Jobs = core::runtime::Jobs;

enum class Storage { Atlas, Standalone };
struct ResolvedImage {
    double x{}, y{}, width{}, height{}; // Exact logical source rectangle, including fractional pixels.
    ImageOptions options;
};
struct ImageAsset final : Asset {
    std::shared_ptr<const Upload::Texture> texture;
    Atlas::Rect content; // Effective pixels in backing, top-left; excludes extrusion.
    ResolvedImage image; // Resolved dimensions/default center. Scale is not baked into pixels.
    std::uint32_t sourceWidth{}, sourceHeight{};
    Storage storage{Storage::Atlas};
    // Logical levels packed vertically into physical level 0, each with its
    // own extrusion. Zero uses the backing's ordinary hardware mip chain.
    std::uint32_t mipLevels{}, mipExtrusion{2};
    // Normalized image-local UV to physical backing. No Y flip or half-texel bias.
    Vec2 MapUV(Vec2 local) const;
};
struct Config {
    std::size_t capacity{8192}, waveItems{128}, activeWaves{2};
    std::uint64_t wavePixels{4 * 1024 * 1024};
    std::uint32_t maxDelayTicks{2}; // Since this wave opened, not original request enqueue.
    std::size_t maxWaveBytes{64 * 1024 * 1024};
    Atlas::Config atlas;
};
struct Budget {
    std::size_t visits{64}, uploadRequests{2}, sourceCollect{8};
};
struct Stats {
    std::size_t pending{}, waves{};
    std::uint64_t started{}, finished{}, failed{}, cancelled{}, wavesBuilt{}, pagesUploaded{};
};
class Provider final : public Resource::Provider {
public:
    // Borrowed services outlive Shutdown. Page memory is separate from source
    // decode credit and has at least 2*maxWaveBytes capacity. Host ticks Upload
    // independently, even while no Lua runs; Shutdown never closes borrowed services.
    Provider(Jobs::Executor&, ImageSources&, Upload::Queue&, Jobs::MemoryBudget pages, Config = {});
    ~Provider();
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
    std::unique_ptr<Operation> Start(Handle, const Request&) override;
    // Once per host frame, NOT once per Operation.Poll. No file/decode/pack or
    // bulk pixel copies on this thread; Poll only observes small result state.
    void Tick(Budget = {});
    Stats GetStats() const;
    void Shutdown() noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
