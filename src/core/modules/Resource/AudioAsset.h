#pragma once
#include "Resource.h"
#include "core/modules/Audio/Audio.h"

namespace core::modules::Resource {
// Anonymous playback source owned by the sole resource registry. Sound/Music
// naming and readiness remain registry concerns; Audio never registers names.
struct AudioAsset final : Asset {
    explicit AudioAsset(std::shared_ptr<Audio::Source> value) : source(std::move(value)) {}
    std::shared_ptr<Audio::Source> source;
    void Retire() const noexcept override { source->Retire(); }
};
}
