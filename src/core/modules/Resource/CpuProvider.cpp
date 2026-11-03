#include "CpuProvider.h"
#include "AudioAsset.h"
#include "Font.h"
#include <algorithm>
#include <list>
#include <stdexcept>
#include <thread>
namespace core::modules::Resource {
namespace {
namespace J=runtime::Jobs;
struct Result {
    std::optional<Audio::PreparedAudio> audio;
    std::shared_ptr<const Asset> asset;
};
struct CpuState {
    Request request;
    std::optional<J::Ticket> ticket; // Owner-thread scheduling/control only.
    std::shared_ptr<Result> result=std::make_shared<Result>();
    bool cancelled{};
};
struct Gate {
    J::Executor& workers;
    std::shared_ptr<const File::FileSystem> files;
    J::MemoryBudget memory;
    Audio::AudioSystem* audio;
    std::thread::id owner{std::this_thread::get_id()};
    bool active{true};
};
class CpuOperation final : public Operation {
public:
    CpuOperation(std::shared_ptr<Gate> g,std::shared_ptr<CpuState> s):gate(std::move(g)),state(std::move(s)) {}
    ~CpuOperation() override { Cancel(); }
    PollResult Poll() override {
        if(gate->owner!=std::this_thread::get_id()) throw std::logic_error("CPU resource operation requires owner thread");
        if(!gate->active || state->cancelled) throw std::runtime_error("CPU resource operation cancelled");
        if(!state->ticket) {
            // Worker only sees detached request/result and immutable services.
            const auto request=state->request;
            state->ticket=gate->workers.TrySubmit([request,result=state->result,files=gate->files,memory=gate->memory](const J::StopToken& stop) {
                stop.Check();
                if(request.id.kind==Kind::Font) result->asset=Fonts::Prepare(files,request.source,memory,stop);
                else {
                    const auto& declaration=std::get<AudioRequest>(request.data);
                    Audio::LoadOptions options{declaration.mode==AudioMode::Decoded?Audio::StorageMode::Decoded:Audio::StorageMode::Stream,{}};
                    if(declaration.loop) options.loop=Audio::LoopSeconds{declaration.loop->start,declaration.loop->finish};
                    result->audio=Audio::AudioSystem::Prepare(files,request.source,options,memory,stop);
                }
                stop.Check();
            });
            return {Stage::Queued,{},{}};
        }
        if(!state->ticket->IsFinished()) return {Stage::Decoding,{},{}};
        state->ticket->CheckResult(); // Synchronizes result publication; no worker touches registry.
        if(state->result->audio) {
            if(!gate->audio) throw std::runtime_error("Audio device service is unavailable");
            state->result->asset=std::make_shared<AudioAsset>(gate->audio->CreateSource(*state->result->audio));
            state->result->audio.reset();
        }
        if(!state->result->asset) throw std::runtime_error("CPU preparation returned no resource");
        return {Stage::Prepared,state->result->asset,{}};
    }
    void Cancel() noexcept override {
        if(gate->owner!=std::this_thread::get_id()) std::terminate();
        state->cancelled=true;
        if(state->ticket) state->ticket->Cancel();
    }
private:
    std::shared_ptr<Gate> gate;
    std::shared_ptr<CpuState> state;
};
}
struct CpuProvider::Impl {
    std::shared_ptr<Gate> gate;
    std::list<std::weak_ptr<CpuState>> active;
};
CpuProvider::CpuProvider(J::Executor& workers,std::shared_ptr<const File::FileSystem> files,
    J::MemoryBudget memory,Audio::AudioSystem* audio)
    :impl(std::make_unique<Impl>()) {
    if(!files) throw std::invalid_argument("CPU resource provider requires VFS");
    impl->gate=std::make_shared<Gate>(Gate{workers,std::move(files),std::move(memory),audio});
}
CpuProvider::~CpuProvider() { Shutdown(); }
std::unique_ptr<Operation> CpuProvider::Start(Handle,const Request& request) {
    if(impl->gate->owner!=std::this_thread::get_id() || !impl->gate->active)
        throw std::logic_error("CPU resource provider requires live owner thread");
    if(request.id.kind!=Kind::Sound && request.id.kind!=Kind::Music && request.id.kind!=Kind::Font)
        throw std::invalid_argument("CPU provider accepts only Sound, Music and Font");
    if(request.id.kind!=Kind::Font && !impl->gate->audio) throw std::runtime_error("Audio service is unavailable");
    impl->active.remove_if([](const auto& weak){ return weak.expired(); });
    auto state=std::make_shared<CpuState>(); state->request=request;
    auto operation=std::make_unique<CpuOperation>(impl->gate,state);
    impl->active.push_back(state);
    return operation;
}
void CpuProvider::Shutdown() noexcept {
    if(!impl || !impl->gate->active) return;
    if(impl->gate->owner!=std::this_thread::get_id()) std::terminate();
    impl->gate->active=false;
    for(const auto& weak:impl->active) if(const auto state=weak.lock()) {
        state->cancelled=true;
        if(state->ticket) state->ticket->Cancel();
    }
    impl->active.clear();
}
}
