#pragma once

// Internal, opt-in benchmark instrumentation. No public API or production state.
#ifdef ENGINE_SPRITE_PROFILE
#include "Renderer.h"
#include <SDL3/SDL_stdinc.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>
#include <string>
#include <vector>

namespace core::modules::Graphic::Render::profile {
using Clock = std::chrono::steady_clock;
enum class Phase { Record, Prepare, Upload, Encode, Acquire, Submit };
struct Sample {
    std::uint64_t frame{};
    double recordMs{}, prepareMs{}, uploadMs{}, encodeMs{}, acquireMs{}, submitMs{};
    double hostFrameIntervalMs{-1};
    std::uint64_t drawCalls{}, sprites{}, textureBindings{}, vertexBytes{}, parameterBytes{}, indexBytes{}, bufferAllocations{};
    std::size_t commandSize{}, vertexSize{}, spriteVertexSize{}, parameterSize{}, indexSize{}, commandCount{};
    std::size_t orderElementSize{}, currentOrderBytes{}, currentOrderCapacityBytes{}, retainedOrderCapacityBytes{};
    std::size_t currentCommandBytes{}, currentGeometryBytes{};
    std::size_t currentCommandCapacityBytes{}, currentGeometryCapacityBytes{};
    std::size_t currentTransferCapacityBytes{}, currentGpuCapacityBytes{};
    std::size_t retainedCommandCapacityBytes{}, retainedGeometryCapacityBytes{};
    std::size_t retainedTransferCapacityBytes{}, retainedGpuCapacityBytes{}, ribbonScratchCapacityBytes{};
    std::size_t inFlight{};
    std::uint64_t backpressure{};
};
struct State {
    std::string path;
    std::vector<Sample> samples;
    Sample current;
    Stats before;
    Clock::time_point previousStart{};
    bool active{}, havePreviousStart{};
};
inline State state;
inline constexpr std::size_t maximumSamples = 1'000'000;
inline double Milliseconds(Clock::duration elapsed) {
    return std::chrono::duration<double, std::milli>(elapsed).count();
}
inline void Initialize() {
    state = {};
    const char* path = SDL_getenv("ENGINE_SPRITE_PROFILE_PATH");
    if (!path || !*path) return;
    state.path = path;
    state.samples.reserve(65536);
}
inline void Begin(std::uint64_t frame, const Stats& before) {
    if (state.path.empty()) return;
    if (state.samples.size() >= maximumSamples)
        throw std::length_error("Sprite profile exceeds 1000000 samples");
    const auto now = Clock::now();
    state.current = {};
    state.current.frame = frame;
    if (state.havePreviousStart) state.current.hostFrameIntervalMs = Milliseconds(now - state.previousStart);
    state.previousStart = now;
    state.havePreviousStart = true;
    state.before = before;
    state.active = true;
}
class Timer {
public:
    explicit Timer(Phase phase, bool excludeUpload = false) : excludeUpload_(excludeUpload) {
        if (!state.active) return;
        switch (phase) {
        case Phase::Record: result_ = &state.current.recordMs; break;
        case Phase::Prepare: result_ = &state.current.prepareMs; break;
        case Phase::Upload: result_ = &state.current.uploadMs; break;
        case Phase::Encode: result_ = &state.current.encodeMs; break;
        case Phase::Acquire: result_ = &state.current.acquireMs; break;
        case Phase::Submit: result_ = &state.current.submitMs; break;
        }
        uploadBefore_ = state.current.uploadMs;
        started_ = Clock::now();
    }
    ~Timer() { Stop(); }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    void Stop() {
        if (!result_) return;
        const double elapsed = Milliseconds(Clock::now() - started_);
        *result_ += elapsed - (excludeUpload_ ? state.current.uploadMs - uploadBefore_ : 0);
        result_ = nullptr;
    }
private:
    double* result_{};
    Clock::time_point started_{};
    double uploadBefore_{};
    bool excludeUpload_{};
};
inline void Finish(const Stats& after) {
    if (!state.active) return;
    auto& sample = state.current;
    sample.drawCalls = after.drawCalls - state.before.drawCalls;
    sample.sprites = after.sprites - state.before.sprites;
    sample.textureBindings = after.textureBindings - state.before.textureBindings;
    sample.vertexBytes = after.vertexBytes - state.before.vertexBytes;
    sample.parameterBytes = after.parameterBytes - state.before.parameterBytes;
    sample.indexBytes = after.indexBytes - state.before.indexBytes;
    sample.bufferAllocations = after.bufferAllocations - state.before.bufferAllocations;
    sample.inFlight = after.inFlight;
    sample.backpressure = after.backpressure - state.before.backpressure;
    state.samples.push_back(sample);
    state.active = false;
}
inline void Flush() {
    state.active = false;
    if (state.path.empty()) return;
    // File creation and all output happen once, outside the measured frame loop.
    std::ofstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.open(std::filesystem::path(std::u8string(state.path.begin(),state.path.end())), std::ios::binary | std::ios::trunc);
    output.imbue(std::locale::classic());
    output << "frame,record_ms,prepare_ms,upload_ms,encode_ms,acquire_ms,submit_ms,hostframeinterval_ms,gpu_ms,"
        "draw_calls,sprites,texture_bindings,vertex_bytes,parameter_bytes,index_bytes,total_upload_bytes,buffer_allocations,command_size,vertex_size,sprite_vertex_size,parameter_size,index_size,command_count,"
        "current_command_bytes,current_geometry_bytes,current_command_capacity_bytes,current_geometry_capacity_bytes,"
        "current_transfer_capacity_bytes,current_gpu_capacity_bytes,retained_command_capacity_bytes,"
        "retained_geometry_capacity_bytes,retained_transfer_capacity_bytes,retained_gpu_capacity_bytes,"
        "ribbon_scratch_capacity_bytes,in_flight,backpressure,order_element_size,current_order_bytes,"
        "current_order_capacity_bytes,retained_order_capacity_bytes,current_frame_cpu_bytes,"
        "current_frame_cpu_capacity_bytes,retained_frame_cpu_capacity_bytes\n";
    output << std::fixed << std::setprecision(6);
    for (const auto& s : state.samples) {
        output << s.frame << ',' << s.recordMs << ',' << s.prepareMs << ',' << s.uploadMs << ','
            << s.encodeMs << ',' << s.acquireMs << ',' << s.submitMs << ',' << s.hostFrameIntervalMs << ",-1,"
            << s.drawCalls << ',' << s.sprites << ',' << s.textureBindings << ',' << s.vertexBytes << ',' << s.parameterBytes << ',' << s.indexBytes << ',' << s.vertexBytes+s.parameterBytes+s.indexBytes << ','
            << s.bufferAllocations << ',' << s.commandSize << ',' << s.vertexSize << ',' << s.spriteVertexSize << ',' << s.parameterSize << ',' << s.indexSize << ',' << s.commandCount << ','
            << s.currentCommandBytes << ',' << s.currentGeometryBytes << ',' << s.currentCommandCapacityBytes << ','
            << s.currentGeometryCapacityBytes << ',' << s.currentTransferCapacityBytes << ',' << s.currentGpuCapacityBytes << ','
            << s.retainedCommandCapacityBytes << ',' << s.retainedGeometryCapacityBytes << ','
            << s.retainedTransferCapacityBytes << ',' << s.retainedGpuCapacityBytes << ',' << s.ribbonScratchCapacityBytes << ','
            << s.inFlight << ',' << s.backpressure << ',' << s.orderElementSize << ',' << s.currentOrderBytes << ','
            << s.currentOrderCapacityBytes << ',' << s.retainedOrderCapacityBytes << ','
            << s.currentCommandBytes+s.currentGeometryBytes+s.currentOrderBytes << ','
            << s.currentCommandCapacityBytes+s.currentGeometryCapacityBytes+s.currentOrderCapacityBytes << ','
            << s.retainedCommandCapacityBytes+s.retainedGeometryCapacityBytes+s.retainedOrderCapacityBytes << '\n';
    }
    output.close();
    state.path.clear();
}
}
#endif
