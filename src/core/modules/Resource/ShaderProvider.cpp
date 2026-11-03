#include "ShaderProvider.h"
#include <list>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace core::modules::Resource {
namespace {
namespace J = runtime::Jobs;
namespace S = Graphic::Shader;
constexpr std::size_t CompileBytes = 64 * 1024 * 1024;
struct CompiledOwner {
    J::Reservation credit; // Release code/model storage before returning credit.
    S::Compiled value;
};
struct Result { std::shared_ptr<const S::Compiled> compiled; };
struct ShaderState {
    Request request;
    std::optional<J::Ticket> ticket;
    std::shared_ptr<Result> result = std::make_shared<Result>();
    std::shared_ptr<const ShaderAsset> asset;
    bool cancelled{};
};
struct Gate {
    J::Executor& workers;
    std::shared_ptr<const File::FileSystem> files;
    J::MemoryBudget memory;
    S::ProgramStore& programs;
    const std::thread::id owner{std::this_thread::get_id()};
    bool live{true};
};
class ShaderOperation final : public Operation {
public:
    ShaderOperation(std::shared_ptr<Gate> gate, std::shared_ptr<ShaderState> state)
        : gate_(std::move(gate)), state_(std::move(state)) {}
    ~ShaderOperation() override { Cancel(); }
    PollResult Poll() override {
        if (gate_->owner != std::this_thread::get_id())
            throw std::logic_error("Shader resource operation requires owner thread");
        if (!gate_->live || state_->cancelled) throw std::runtime_error("Shader resource operation cancelled");
        if (!state_->ticket) {
            const auto request = state_->request;
            state_->ticket = gate_->workers.TrySubmit(
                [request, result = state_->result, files = gate_->files, memory = gate_->memory](const J::StopToken& stop) mutable {
                    stop.Check();
                    // Reserve before compiler output allocation. This accounts
                    // engine-owned source/code/model buffers; DXC's internal
                    // heap is outside this budget and is not a process-RSS cap.
                    auto reservation = memory.TryAcquire(CompileBytes, stop);
                    if (!reservation) throw std::runtime_error("Shader compile memory budget has insufficient available capacity");
                    auto owner = std::make_shared<CompiledOwner>();
                    owner->credit = std::move(*reservation);
                    owner->value = S::Compile(files, request.source, [&stop] { return stop.IsStopRequested(); });
                    stop.Check();
                    const auto resident = owner->value.ResidentBytes();
                    if (resident > CompileBytes) throw std::length_error("Shader compiled code/model exceeds the 64 MiB resource limit");
                    owner->credit.Shrink(resident);
                    result->compiled = std::shared_ptr<const S::Compiled>(owner, &owner->value);
                    stop.Check();
                });
            return {Stage::Queued, {}, {}};
        }
        if (!state_->ticket->IsFinished()) return {Stage::Decoding, {}, {}};
        state_->ticket->CheckResult(); // Acquires the worker completion fence.
        if (!state_->asset) {
            if (!state_->result->compiled) throw std::runtime_error("Shader compiler returned no prepared program");
            // A failed SDL shader or baseline-pipeline creation never reaches
            // Prepared and therefore cannot pass Resource's atomic publish gate.
            state_->asset = std::make_shared<ShaderAsset>(gate_->programs.Create(state_->result->compiled));
            state_->result->compiled.reset();
        }
        return {Stage::Prepared, state_->asset, {}};
    }
    void Cancel() noexcept override {
        if (gate_->owner != std::this_thread::get_id()) std::terminate();
        state_->cancelled = true;
        if (state_->ticket) state_->ticket->Cancel();
    }
private:
    std::shared_ptr<Gate> gate_;
    std::shared_ptr<ShaderState> state_;
};
}
struct ShaderProvider::Impl {
    std::shared_ptr<Gate> gate;
    std::list<std::weak_ptr<ShaderState>> active;
};
ShaderProvider::ShaderProvider(J::Executor& workers, std::shared_ptr<const File::FileSystem> files,
    J::MemoryBudget memory, S::ProgramStore& programs) : impl_(std::make_unique<Impl>()) {
    if (!files) throw std::invalid_argument("Shader resource provider requires VFS");
    impl_->gate = std::make_shared<Gate>(Gate{workers, std::move(files), std::move(memory), programs});
}
ShaderProvider::~ShaderProvider() { Shutdown(); }
std::unique_ptr<Operation> ShaderProvider::Start(Handle, const Request& request) {
    if (impl_->gate->owner != std::this_thread::get_id() || !impl_->gate->live)
        throw std::logic_error("Shader resource provider requires live owner thread");
    if (request.id.kind != Kind::Shader || !std::holds_alternative<FileRequest>(request.data))
        throw std::invalid_argument("Shader resource provider accepts only Shader file requests");
    impl_->active.remove_if([](const auto& state) { return state.expired(); });
    auto state = std::make_shared<ShaderState>();
    state->request = request;
    auto operation = std::make_unique<ShaderOperation>(impl_->gate, state);
    impl_->active.push_back(state);
    return operation;
}
void ShaderProvider::Shutdown() noexcept {
    if (!impl_ || !impl_->gate->live) return;
    if (impl_->gate->owner != std::this_thread::get_id()) std::terminate();
    impl_->gate->live = false;
    for (const auto& weak : impl_->active) if (auto state = weak.lock()) {
        state->cancelled = true;
        if (state->ticket) state->ticket->Cancel();
    }
    impl_->active.clear();
}
}
