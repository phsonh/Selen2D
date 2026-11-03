#include "Decoder.h"
#include <algorithm>
#include <stdexcept>
namespace core::modules::Audio::detail {
LoopingReader::LoopingReader(Decoder& decoder) : decoder_(decoder), info_(decoder.GetInfo()) {}
void LoopingReader::SetLoop(std::optional<LoopRange> range) {
    if (range && (range->begin >= range->end || range->end > info_.frames))
        throw std::invalid_argument("audio loop must satisfy 0 <= begin < end <= source frames");
    loop_ = range;
}
void LoopingReader::Reset(std::uint64_t frame) {
    if (frame > info_.frames || (loop_ && frame >= loop_->end))
        throw std::invalid_argument("audio start frame is outside the playback range");
    decoder_.Seek(frame);
    position_ = frame;
}
std::size_t LoopingReader::Read(std::span<float> destination) {
    if (destination.size() % 2) throw std::invalid_argument("PCM destination must contain stereo frames");
    std::size_t written{};
    const auto requested = destination.size() / 2;
    while (written < requested) {
        const auto end = loop_ ? loop_->end : info_.frames;
        if (position_ >= end) {
            if (!loop_) break;
            position_ = loop_->begin;
            decoder_.Seek(position_);
        }
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(requested-written, end-position_));
        const auto actual = decoder_.Read(destination.subspan(written*2, count*2));
        if (!actual || actual > count) throw std::runtime_error("audio decoder ended before its declared PCM length");
        written += actual;
        position_ += actual;
    }
    return written;
}
}
