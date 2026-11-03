#pragma once
#include "Audio.h"
// Native diagnostics only, deliberately not part of the Lua API. DeviceId
// enables SDL's actual final-mix PCM tap rather than a mock of pan/gain math.
namespace core::modules::Audio::testing {
struct Snapshot {
    PlaybackState state{};
    std::uint64_t generation{},submittedFrame{};
    std::size_t prefetchedFrames{};
    int streamQueuedBytes{},streamAvailableBytes{};
    bool hasTransport{};
    float effectiveGain{};
};
struct Access {
    static std::uint32_t DeviceId(const AudioSystem& audio);
    static bool HasDecoderWorker(const AudioSystem& audio);
    static void PauseDecoder(AudioSystem& audio,bool pause);
    static Snapshot Inspect(const Playback& playback);
};
}
