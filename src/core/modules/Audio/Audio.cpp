#include "Audio.h"
#include "Decoder.h"
#include "AudioTestAccess.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace core::modules::Audio::detail {
namespace {
constexpr std::size_t RingFrames=8192, BlockFrames=2048;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
void Check(bool success,const char* operation) {
    if(!success) throw std::runtime_error(std::string(operation)+": "+SDL_GetError());
}
void GainValue(double value) {
    if(!std::isfinite(value)||value<0||value>1) throw std::invalid_argument("audio gain must be finite and between 0 and 1");
}
void PanValue(float value) {
    if(!std::isfinite(value)||value < -1||value>1) throw std::invalid_argument("audio pan must be finite and between -1 and 1");
}
void Name(const std::string& value,const char* kind) {
    if(value.empty()||value.find('\0')!=std::string::npos) throw std::invalid_argument(std::string(kind)+" must be a nonempty name without NUL");
}
bool Terminal(PlaybackState value) {
    return value==PlaybackState::Stopped||value==PlaybackState::Finished||value==PlaybackState::Failed||value==PlaybackState::Destroyed;
}
std::uint64_t FrameAt(double seconds,const ResourceInfo& info,bool loop) {
    if(!std::isfinite(seconds)||seconds<0||seconds>info.duration) throw std::invalid_argument("audio position is outside the source duration");
    const auto frame=static_cast<std::uint64_t>(std::round(seconds*info.sampleRate));
    if(frame>info.frames||(loop&&frame>=info.loopEndFrame)) throw std::invalid_argument("audio position is outside the enabled loop range");
    return frame;
}
std::uint64_t Advance(std::uint64_t position,std::size_t count,const ResourceInfo& info,bool loop) noexcept {
    if(!loop) return std::min(info.frames,position+count);
    while(count) {
        if(position>=info.loopEndFrame) position=info.loopStartFrame;
        const auto step=std::min<std::uint64_t>(count,info.loopEndFrame-position);
        position+=step; count-=static_cast<std::size_t>(step);
    }
    return position;
}
}
struct Record {
    std::shared_ptr<const ResourceData> data;
    std::weak_ptr<System> system;
    std::atomic<double> volume{1};
    std::atomic<bool> retired{};
    std::uint64_t generation{};
};

struct Transport {
    explicit Transport(std::shared_ptr<const Record> record,bool loop)
        : record(std::move(record)),loop(loop) {
        if(this->record->data->info.mode==StorageMode::Stream) ring=std::make_unique<float[]>(RingFrames*2);
    }
    std::shared_ptr<const Record> record;
    bool loop{};
    SDL_AudioStream* stream{};
    bool bound{}; // Control lock only.
    std::mutex producer; // Never acquired by the realtime callback.
    std::unique_ptr<float[]> ring;
    std::atomic<std::uint64_t> read{},write{},eofGeneration{},flushedGeneration{},readyGeneration{};
    std::uint64_t appliedGeneration{}; // Control only; prime applies once, not on ordinary Resume.
    std::array<float,BlockFrames*2> scratch{}; // Callback only, preallocated.
    std::uint64_t decodedCursor{}; // Callback, or control after Unbind.
    std::unique_ptr<Decoder> decoder; // Worker exclusively, disposed by owner after inflight=false.
    std::unique_ptr<LoopingReader> reader;
    std::uint64_t decoderGeneration{};
    bool inflight{}; // System control lock.
    std::atomic<bool> fault{};
    std::array<char,256> faultText{};
    Voice* owner{}; // Non-owning, service retains owner until callback has been detached.
};
struct Voice {
    std::weak_ptr<System> system;
    std::atomic<PlaybackState> state{PlaybackState::Starting};
    std::atomic<std::uint64_t> generation{1},submitted{};
    std::atomic<float> volume{1},pan{};
    ResourceInfo info;
    std::string bus,error;
    std::uint64_t target{};
    bool pausedIntent{};
    std::unique_ptr<Transport> transport;
};
struct System {
    const std::thread::id owner{std::this_thread::get_id()};
    mutable std::mutex control;
    std::condition_variable changed;
    SDL_AudioDeviceID device{};
    SDL_AudioSpec output{};
    bool alive{},exit{},decoderPaused{};
    float master{1};
    std::uint64_t nextResourceGeneration{1};
    std::unordered_map<std::string,double> buses;
    std::vector<std::shared_ptr<Voice>> active;
    std::thread worker;
    std::size_t nextVoice{};
    // Fixed storage: reporting failure on the service worker must not itself
    // allocate or grow an unbounded diagnostics queue. Control lock protects it.
    std::array<std::array<char,2048>,32> errors{};
    std::size_t errorHead{},errorCount{},droppedErrors{};

    double BusGain(const std::string& bus) const { const auto it=buses.find(bus); return it==buses.end()?1:it->second; }
    void RequireAlive() const { if(!alive) throw std::runtime_error("audio subsystem is shut down"); }
    void ApplyGain(Voice& voice) {
        if(voice.transport&&voice.transport->stream)
            Check(SDL_SetAudioStreamGain(voice.transport->stream,static_cast<float>(master*BusGain(voice.bus)*voice.volume.load()
                *voice.transport->record->volume.load())),"set audio playback gain");
    }
    void Unbind(Transport& value) {
        if(value.bound) { SDL_UnbindAudioStream(value.stream); value.bound=false; }
    }
    void Invalidate(Voice& voice) {
        voice.generation.fetch_add(1,std::memory_order_acq_rel);
        if(!voice.transport) return;
        auto& value=*voice.transport;
        Unbind(value); // No producer/stream lock: follows SDL's device -> stream ordering.
        std::lock_guard commit(value.producer);
        value.read.store(0); value.write.store(0);
        value.eofGeneration.store(0); value.flushedGeneration.store(0); value.readyGeneration.store(0);
        value.fault.store(false);
        if(value.stream) Check(SDL_ClearAudioStream(value.stream),"clear audio playback");
    }
    void Stop(Voice& voice,PlaybackState state=PlaybackState::Stopped) {
        if(Terminal(voice.state.load())) return;
        Invalidate(voice);
        voice.state.store(state,std::memory_order_release);
        changed.notify_all();
    }
    void Report(const Voice* voice,const char* error) noexcept {
        if(errorCount==errors.size()) { if(droppedErrors!=static_cast<std::size_t>(-1)) ++droppedErrors; return; }
        auto& message=errors[(errorHead+errorCount)%errors.size()];
        const char* path=voice&&voice->transport ? voice->transport->record->data->path.c_str() : "<audio service>";
        SDL_snprintf(message.data(),message.size(),"audio '%.768s': %.1200s",path,error);
        ++errorCount;
    }
    void Fail(Voice& voice,const std::string& error) noexcept {
        if(Terminal(voice.state.load())) return;
        Report(&voice,error.c_str());
        try { voice.error=error; } catch(...) { /* Fixed queue still holds the error. */ }
        try { Stop(voice,PlaybackState::Failed); }
        catch(...) {
            // Invalidate detaches before the fallible SDL queue-clear operation.
            voice.state.store(PlaybackState::Failed,std::memory_order_release);
            changed.notify_all();
        }
    }
    void Refresh(Voice& voice) {
        if(!voice.transport||Terminal(voice.state.load())) return;
        auto& value=*voice.transport;
        if(value.record->retired.load(std::memory_order_acquire)) { Stop(voice); return; }
        if(value.fault.load(std::memory_order_acquire)) {
            // The callback can report another failure on its next pull. Wait
            // for it to leave before copying its non-atomic diagnostic text.
            Unbind(value);
            const std::string error=value.faultText.data();
            Fail(voice,error);
            return;
        }
        const auto generation=voice.generation.load(std::memory_order_acquire);
        if(value.readyGeneration.load(std::memory_order_acquire)==generation&&value.appliedGeneration!=generation) {
            if(voice.state.load()==PlaybackState::Starting||voice.state.load()==PlaybackState::Paused) {
                voice.submitted.store(voice.target,std::memory_order_release);
                value.appliedGeneration=generation;
                if(voice.pausedIntent) voice.state.store(PlaybackState::Paused);
                else {
                    // Publish Playing before Bind, so the first SDL pull can supply PCM.
                    voice.state.store(PlaybackState::Playing,std::memory_order_release);
                    if(!value.bound) {
                        if(!SDL_BindAudioStream(device,value.stream)) { Fail(voice,std::string("bind audio playback: ")+SDL_GetError()); return; }
                        value.bound=true;
                    }
                }
            }
        }
        if(value.flushedGeneration.load(std::memory_order_acquire)==generation
            && value.read.load(std::memory_order_acquire)==value.write.load(std::memory_order_acquire)) {
            const auto queued=SDL_GetAudioStreamQueued(value.stream);
            const auto available=SDL_GetAudioStreamAvailable(value.stream);
            if(queued<0||available<0) { Fail(voice,std::string("query audio drain: ")+SDL_GetError()); return; }
            if(voice.state.load()==PlaybackState::Playing&&queued==0&&available==0) Stop(voice,PlaybackState::Finished);
        }
    }
    void Pause(Voice& voice) {
        if(Terminal(voice.state.load())) return;
        voice.pausedIntent=true; // Set before Refresh could bind a primed voice.
        Refresh(voice);
        if(Terminal(voice.state.load())) return;
        Unbind(*voice.transport);
        voice.state.store(PlaybackState::Paused,std::memory_order_release);
    }
    void Resume(Voice& voice) {
        if(Terminal(voice.state.load())||voice.state.load()!=PlaybackState::Paused) return;
        voice.pausedIntent=false;
        auto& value=*voice.transport;
        if(value.readyGeneration.load()!=voice.generation.load()
            ||value.appliedGeneration!=voice.generation.load()) voice.state.store(PlaybackState::Starting);
        else {
            voice.state.store(PlaybackState::Playing,std::memory_order_release);
            if(!SDL_BindAudioStream(device,value.stream)) { Fail(voice,std::string("resume audio: ")+SDL_GetError()); return; }
            value.bound=true;
        }
        Refresh(voice);
        changed.notify_all();
    }
    void Reclaim() {
        for(auto it=active.begin();it!=active.end();) {
            auto& voice=**it;
            if(Terminal(voice.state.load())&&(!voice.transport||!voice.transport->inflight)) {
                if(voice.transport) {
                    auto& value=*voice.transport;
                    Unbind(value);
                    if(value.stream) {
                        SDL_SetAudioStreamGetCallback(value.stream,nullptr,nullptr);
                        SDL_DestroyAudioStream(value.stream);
                    }
                    value.stream=nullptr;
                    voice.transport.reset();
                }
                it=active.erase(it);
            } else ++it;
        }
    }
    void Run() noexcept;
    void RunWorker();
};

namespace {
void CallbackFault(Transport& value,const char* operation) noexcept {
    SDL_snprintf(value.faultText.data(),value.faultText.size(),"%s: %s",operation,SDL_GetError());
    value.fault.store(true,std::memory_order_release);
}
std::size_t ReadDecoded(Transport& value,std::span<float> out) noexcept {
    const auto& data=*value.record->data;
    std::size_t copied{};
    const auto requested=out.size()/2;
    while(copied<requested) {
        const auto end=value.loop?data.info.loopEndFrame:data.info.frames;
        if(value.decodedCursor>=end) {
            if(!value.loop) break;
            value.decodedCursor=data.info.loopStartFrame;
        }
        const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(requested-copied,end-value.decodedCursor));
        std::memcpy(out.data()+copied*2,data.pcm.data()+value.decodedCursor*2,count*2*sizeof(float));
        value.decodedCursor+=count; copied+=count;
    }
    return copied;
}
void SDLCALL Supply(void* userdata,SDL_AudioStream* stream,int additional,int) noexcept {
    auto& value=*static_cast<Transport*>(userdata);
    auto& voice=*value.owner;
    if(voice.state.load(std::memory_order_acquire)!=PlaybackState::Playing) return;
    if(value.record->retired.load(std::memory_order_acquire)) return;
    const auto generation=voice.generation.load(std::memory_order_acquire);
    const auto decoded=value.record->data->info.mode==StorageMode::Decoded;
    std::size_t remaining=additional>0 ? (static_cast<std::size_t>(additional)+sizeof(float)*2-1)/(sizeof(float)*2) : 0;
    // The engine only touches preallocated buffers. SDL's Put/convert internals
    // may allocate; SDL does not offer an end-to-end hard-realtime guarantee.
    while(remaining&&voice.generation.load(std::memory_order_acquire)==generation) {
        const auto wanted=std::min(remaining,BlockFrames);
        std::size_t count{};
        std::uint64_t read{};
        if(decoded) count=ReadDecoded(value,std::span(value.scratch).first(wanted*2));
        else {
            read=value.read.load(std::memory_order_relaxed);
            const auto write=value.write.load(std::memory_order_acquire);
            count=static_cast<std::size_t>(std::min<std::uint64_t>(wanted,write-read));
            for(std::size_t i=0;i<count;++i) {
                const auto offset=static_cast<std::size_t>((read+i)%RingFrames)*2;
                value.scratch[2*i]=value.ring[offset]; value.scratch[2*i+1]=value.ring[offset+1];
            }
        }
        if(!count) break;
        const auto pan=voice.pan.load(std::memory_order_relaxed);
        const auto left=pan>0?1-pan:1;
        const auto right=pan<0?1+pan:1;
        for(std::size_t i=0;i<count;++i) { value.scratch[2*i]*=left; value.scratch[2*i+1]*=right; }
        if(!SDL_PutAudioStreamData(stream,value.scratch.data(),static_cast<int>(count*2*sizeof(float)))) { CallbackFault(value,"queue audio PCM"); return; }
        if(!decoded) value.read.store(read+count,std::memory_order_release);
        voice.submitted.store(Advance(voice.submitted.load(),count,voice.info,value.loop),std::memory_order_release);
        remaining-=count;
    }
    const bool eof=decoded ? (!value.loop&&value.decodedCursor>=voice.info.frames)
        : value.eofGeneration.load(std::memory_order_acquire)==generation
          && value.read.load(std::memory_order_acquire)==value.write.load(std::memory_order_acquire);
    if(eof&&value.flushedGeneration.load(std::memory_order_relaxed)!=generation) {
        if(!SDL_FlushAudioStream(stream)) { CallbackFault(value,"flush audio EOF"); return; }
        value.flushedGeneration.store(generation,std::memory_order_release);
    }
}
struct LockedVoice {
    std::shared_ptr<System> system;
    std::unique_lock<std::mutex> lock;
    explicit LockedVoice(const Voice& voice) : system(voice.system.lock()),lock(system?std::unique_lock(system->control):std::unique_lock<std::mutex>()) {}
};
void RequireControl(const Voice& voice) {
    if(voice.state.load()==PlaybackState::Destroyed) throw std::runtime_error("audio Playback has been destroyed");
}
}

void System::Run() noexcept {
    try { RunWorker(); }
    catch(const std::exception& failure) {
        std::lock_guard lock(control);
        Report(nullptr,failure.what());
        for(const auto& voice:active) {
            try { Stop(*voice,PlaybackState::Failed); }
            catch(...) { voice->state.store(PlaybackState::Failed,std::memory_order_release); }
            if(voice->transport) voice->transport->inflight=false;
        }
    } catch(...) {
        std::lock_guard lock(control);
        Report(nullptr,"unknown exception on audio service worker");
        for(const auto& voice:active) {
            try { Stop(*voice,PlaybackState::Failed); }
            catch(...) { voice->state.store(PlaybackState::Failed,std::memory_order_release); }
            if(voice->transport) voice->transport->inflight=false;
        }
    }
}
void System::RunWorker() {
    std::array<float,BlockFrames*2> scratch{};
    std::unique_lock lock(control);
    for(;;) {
        if(exit) return;
        for(const auto& voice:active) Refresh(*voice);
        Reclaim();
        std::shared_ptr<Voice> selected;
        for(std::size_t checked=0;!decoderPaused&&checked<active.size();++checked) {
            const auto index=(nextVoice+checked)%active.size();
            auto& candidate=*active[index];
            if(Terminal(candidate.state.load())||!candidate.transport) continue;
            auto& value=*candidate.transport;
            const auto generation=candidate.generation.load();
            if(value.record->data->info.mode==StorageMode::Stream&&!value.inflight
                &&value.eofGeneration.load()!=generation
                &&value.write.load(std::memory_order_acquire)-value.read.load(std::memory_order_acquire)<RingFrames) {
                selected=active[index]; nextVoice=(index+1)%active.size(); break;
            }
        }
        if(!selected) { changed.wait_for(lock,std::chrono::milliseconds(3)); continue; }
        auto& voice=*selected;
        auto& value=*voice.transport;
        const auto generation=voice.generation.load();
        const auto target=voice.target;
        const auto data=value.record->data;
        value.inflight=true;
        lock.unlock();
        std::string error;
        try {
            if(value.decoderGeneration!=generation) {
                if(!value.decoder) {
                    // Build the pair transactionally. A failed old-generation
                    // initialization may be superseded by Seek, so it must not
                    // leave a decoder without its reader for the next job.
                    auto decoder=OpenDecoder(data->files,data->path);
                    const auto actual=decoder->GetInfo();
                    if(actual.sampleRate!=data->info.sampleRate||actual.channels!=data->info.channels||actual.frames!=data->info.frames)
                        throw std::runtime_error("audio source changed after Prepare; explicitly Unload/Load it");
                    auto reader=std::make_unique<LoopingReader>(*decoder);
                    reader->SetLoop(value.loop?std::optional(LoopRange{data->info.loopStartFrame,data->info.loopEndFrame}):std::nullopt);
                    value.decoder=std::move(decoder);
                    value.reader=std::move(reader);
                }
                value.reader->Reset(target);
                value.decoderGeneration=generation;
            }
            const auto write=value.write.load(std::memory_order_acquire);
            const auto read=value.read.load(std::memory_order_acquire);
            const auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(BlockFrames,RingFrames-(write-read)));
            const auto count=value.reader->Read(std::span(scratch).first(wanted*2));
            const bool eof=!value.loop&&value.reader->GetPosition()==data->info.frames;
            std::lock_guard commit(value.producer);
            if(voice.generation.load(std::memory_order_acquire)==generation&&!Terminal(voice.state.load())) {
                const auto head=value.write.load(std::memory_order_relaxed);
                for(std::size_t i=0;i<count;++i) {
                    const auto offset=static_cast<std::size_t>((head+i)%RingFrames)*2;
                    value.ring[offset]=scratch[2*i]; value.ring[offset+1]=scratch[2*i+1];
                }
                value.write.store(head+count,std::memory_order_release);
                if(eof) value.eofGeneration.store(generation,std::memory_order_release);
                // First nonempty block (or immediate EOF) is enough to prime.
                value.readyGeneration.store(generation,std::memory_order_release);
            }
        } catch(const std::exception& failure) { error=failure.what(); }
        lock.lock();
        value.inflight=false;
        if(!error.empty()&&voice.generation.load()==generation&&!Terminal(voice.state.load())) Fail(voice,error);
        changed.notify_all();
    }
}
}

namespace core::modules::Audio {
PreparedAudio::PreparedAudio(std::shared_ptr<const detail::ResourceData> data) : data_(std::move(data)) {}
ResourceInfo PreparedAudio::GetInfo() const { return data_->info; }
Source::Source(std::shared_ptr<detail::Record> record) : record_(std::move(record)) {}
ResourceInfo Source::GetInfo() const {
    auto info=record_->data->info;
    info.generation=record_->generation;
    return info;
}
void Source::SetVolume(double volume) {
    detail::GainValue(volume);
    const auto system=record_->system.lock();
    if(!system) throw std::runtime_error("audio source service no longer exists");
    std::lock_guard lock(system->control);
    system->RequireAlive();
    if(record_->retired.load()) throw std::runtime_error("audio source has been retired");
    record_->volume.store(volume);
    for(const auto& voice:system->active)
        if(voice->transport&&voice->transport->record==record_) system->ApplyGain(*voice);
}
double Source::GetVolume() const { return record_->volume.load(); }
void Source::Retire() noexcept {
    record_->retired.store(true,std::memory_order_release);
    const auto system=record_->system.lock();
    if(!system) return;
    try {
        std::lock_guard lock(system->control);
        for(const auto& voice:system->active) if(voice->transport&&voice->transport->record==record_) {
            // Stop detaches the SDL callback before clearing its queue. Even
            // a failed SDL clear must not prevent retiring the other voices.
            try { system->Stop(*voice); }
            catch(const std::exception& error) {
                system->Report(voice.get(),error.what());
                voice->state.store(PlaybackState::Stopped,std::memory_order_release);
            } catch(...) {
                system->Report(voice.get(),"unknown error while retiring audio source");
                voice->state.store(PlaybackState::Stopped,std::memory_order_release);
            }
        }
        system->changed.notify_all();
    } catch(...) { /* Retired is already visible to the callback/worker. */ }
}
Playback::Playback(std::shared_ptr<detail::Voice> voice) : voice_(std::move(voice)) {}
void Playback::Pause() {
    detail::LockedVoice locked(*voice_); detail::RequireControl(*voice_);
    if(locked.system) locked.system->Pause(*voice_);
}
void Playback::Resume() {
    detail::LockedVoice locked(*voice_); detail::RequireControl(*voice_);
    if(locked.system) locked.system->Resume(*voice_);
}
void Playback::Stop() {
    detail::LockedVoice locked(*voice_); detail::RequireControl(*voice_);
    if(locked.system) locked.system->Stop(*voice_);
}
void Playback::Destroy() {
    detail::LockedVoice locked(*voice_);
    if(voice_->state.load()==PlaybackState::Destroyed) return;
    if(locked.system) locked.system->Stop(*voice_);
    voice_->state.store(PlaybackState::Destroyed,std::memory_order_release);
}
void Playback::Seek(double seconds) {
    detail::LockedVoice locked(*voice_); detail::RequireControl(*voice_);
    if(!locked.system||detail::Terminal(voice_->state.load())) throw std::runtime_error("cannot Seek a terminal audio Playback");
    auto& value=*voice_->transport;
    const auto target=detail::FrameAt(seconds,voice_->info,value.loop);
    const bool paused=voice_->pausedIntent||voice_->state.load()==PlaybackState::Paused;
    locked.system->Invalidate(*voice_);
    voice_->target=target; voice_->pausedIntent=paused;
    voice_->state.store(paused?PlaybackState::Paused:PlaybackState::Starting,std::memory_order_release);
    if(value.record->data->info.mode==StorageMode::Decoded) {
        value.decodedCursor=target;
        value.readyGeneration.store(voice_->generation.load(),std::memory_order_release);
    }
    locked.system->changed.notify_all();
}
void Playback::SetVolume(float volume) {
    detail::GainValue(volume); detail::LockedVoice locked(*voice_); detail::RequireControl(*voice_);
    voice_->volume.store(volume);
    if(locked.system) locked.system->ApplyGain(*voice_);
}
float Playback::GetVolume() const { return voice_->volume.load(); }
void Playback::SetPan(float pan) { detail::PanValue(pan); detail::LockedVoice locked(*voice_); detail::RequireControl(*voice_); voice_->pan.store(pan); }
float Playback::GetPan() const { return voice_->pan.load(); }
PlaybackState Playback::GetState() const {
    return voice_->state.load();
}
Position Playback::GetPosition() const {
    return {static_cast<double>(voice_->submitted.load())/voice_->info.sampleRate,"submitted"};
}
std::string Playback::GetError() const { detail::LockedVoice locked(*voice_); return voice_->error; }

AudioSystem::AudioSystem() : system_(std::make_shared<detail::System>()) {}
AudioSystem::~AudioSystem() { Shutdown(); }
void AudioSystem::SetFileSystem(std::shared_ptr<const File::FileSystem> files) {
    if(!files) throw std::invalid_argument("audio requires a virtual file system");
    if(IsInitialized()) throw std::logic_error("audio file system must be configured before Initialize");
    files_=std::move(files);
}
void AudioSystem::Initialize() {
    if(IsInitialized()) return;
    if(!files_) throw std::logic_error("audio file system must be configured before Initialize");
    system_=std::make_shared<detail::System>();
    detail::Check(SDL_InitSubSystem(SDL_INIT_AUDIO),"initialize SDL audio");
    system_->device=SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,nullptr);
    if(!system_->device) {
        const std::string error=SDL_GetError(); SDL_QuitSubSystem(SDL_INIT_AUDIO);
        throw std::runtime_error("open SDL audio device: "+error);
    }
    if(!SDL_GetAudioDeviceFormat(system_->device,&system_->output,nullptr)) {
        const std::string error=SDL_GetError(); SDL_CloseAudioDevice(system_->device); SDL_QuitSubSystem(SDL_INIT_AUDIO);
        system_->device=0; throw std::runtime_error("query SDL audio output format: "+error);
    }
    system_->alive=true;
    try { system_->worker=std::thread([system=system_.get()] { system->Run(); }); }
    catch(...) { system_->alive=false; SDL_CloseAudioDevice(system_->device); system_->device=0; SDL_QuitSubSystem(SDL_INIT_AUDIO); throw; }
}
bool AudioSystem::IsInitialized() const { std::lock_guard lock(system_->control); return system_->alive; }
void AudioSystem::Shutdown() {
    auto system=system_;
    {
        std::lock_guard lock(system->control);
        if(!system->alive) { files_.reset(); return; }
        system->alive=false; system->exit=true;
        for(const auto& voice:system->active) {
            try { system->Stop(*voice); }
            catch(const std::exception& error) {
                system->Report(voice.get(),error.what());
                voice->state.store(PlaybackState::Stopped,std::memory_order_release);
            } catch(...) {
                system->Report(voice.get(),"unknown error while shutting down audio");
                voice->state.store(PlaybackState::Stopped,std::memory_order_release);
            }
        }
    }
    system->changed.notify_all();
    if(system->worker.joinable()) system->worker.join();
    {
        std::lock_guard lock(system->control);
        system->Reclaim();
        SDL_CloseAudioDevice(system->device); system->device=0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    files_.reset();
}
PreparedAudio AudioSystem::Prepare(std::shared_ptr<const File::FileSystem> files,
    const std::string& path,const LoadOptions& options) {
    return PreparedAudio(detail::PrepareResource(std::move(files),path,options));
}
PreparedAudio AudioSystem::Prepare(std::shared_ptr<const File::FileSystem> files,
    const std::string& path,const LoadOptions& options,
    core::runtime::Jobs::MemoryBudget memory,const core::runtime::Jobs::StopToken& stop) {
    return PreparedAudio(detail::PrepareResource(std::move(files),path,options,std::move(memory),stop));
}
std::shared_ptr<Source> AudioSystem::CreateSource(const PreparedAudio& prepared) {
    if(!prepared.data_) throw std::invalid_argument("prepared audio is empty");
    std::lock_guard lock(system_->control); system_->RequireAlive();
    auto record=std::make_shared<detail::Record>();
    record->data=prepared.data_; record->system=system_;
    record->generation=system_->nextResourceGeneration++;
    return std::shared_ptr<Source>(new Source(std::move(record)));
}
void AudioSystem::CheckSource(const std::shared_ptr<Source>& source) const {
    if(!source) throw std::invalid_argument("audio source cannot be null");
    if(source->record_->system.lock()!=system_) throw std::invalid_argument("audio source belongs to another service");
}
void AudioSystem::StopSource(const std::shared_ptr<Source>& source) {
    CheckSource(source);
    std::lock_guard lock(system_->control);
    for(const auto& voice:system_->active)
        if(voice->transport&&voice->transport->record==source->record_) system_->Stop(*voice);
}
void AudioSystem::PauseSource(const std::shared_ptr<Source>& source) {
    CheckSource(source);
    std::lock_guard lock(system_->control);
    for(const auto& voice:system_->active)
        if(voice->transport&&voice->transport->record==source->record_) system_->Pause(*voice);
}
void AudioSystem::ResumeSource(const std::shared_ptr<Source>& source) {
    CheckSource(source);
    std::lock_guard lock(system_->control);
    for(const auto& voice:system_->active)
        if(voice->transport&&voice->transport->record==source->record_) system_->Resume(*voice);
}
std::shared_ptr<Playback> AudioSystem::Play(const std::shared_ptr<Source>& source,const PlayOptions& options) {
    CheckSource(source);
    detail::GainValue(options.volume); detail::PanValue(options.pan); detail::Name(options.bus,"audio bus");
    if(options.policy!=PlayPolicy::Restart&&options.policy!=PlayPolicy::Overlap) throw std::invalid_argument("invalid audio play policy");
    std::lock_guard lock(system_->control); system_->RequireAlive();
    auto record=source->record_;
    if(record->retired.load(std::memory_order_acquire)) throw std::runtime_error("audio source has been retired");
    const auto target=detail::FrameAt(options.position,record->data->info,options.loop);
    auto voice=std::make_shared<detail::Voice>();
    voice->system=system_; voice->info=record->data->info;
    voice->bus=options.bus; voice->target=target;
    voice->volume.store(options.volume); voice->pan.store(options.pan); voice->submitted.store(target);
    voice->transport=std::make_unique<detail::Transport>(record,options.loop);
    auto& value=*voice->transport;
    value.owner=voice.get();
    const SDL_AudioSpec sourceFormat{SDL_AUDIO_F32,2,voice->info.sampleRate};
    value.stream=SDL_CreateAudioStream(&sourceFormat,&system_->output);
    detail::Check(value.stream!=nullptr,"create audio Playback stream");
    struct CreationRollback {
        detail::System& system;
        const std::shared_ptr<detail::Voice>& voice;
        bool committed{};
        ~CreationRollback() noexcept {
            if(committed) return;
            // The outer control lock is still held: the worker cannot have
            // picked this newly created transport for decoding. Detach a
            // possible SDL callback before freeing its userdata. There is no
            // reason to Clear a stream that is being destroyed; Stop/Clear may
            // fail and must never replace the original construction exception.
            voice->generation.fetch_add(1,std::memory_order_acq_rel);
            voice->state.store(PlaybackState::Stopped,std::memory_order_release);
            auto& transport=*voice->transport;
            system.Unbind(transport);
            SDL_SetAudioStreamGetCallback(transport.stream,nullptr,nullptr);
            SDL_DestroyAudioStream(transport.stream);
            transport.stream=nullptr;
            std::erase(system.active,voice);
            voice->transport.reset();
        }
    } rollback{*system_,voice};
    detail::Check(SDL_SetAudioStreamGetCallback(value.stream,detail::Supply,&value),"attach audio callback");
    system_->buses.try_emplace(options.bus,1.0);
    system_->ApplyGain(*voice);
    auto handle=std::shared_ptr<Playback>(new Playback(voice));
    system_->active.push_back(voice);
    if(options.policy==PlayPolicy::Restart)
        for(const auto& old:system_->active)
            if(old!=voice&&old->transport&&old->transport->record==record) system_->Stop(*old);
    if(voice->info.mode==StorageMode::Decoded) {
        value.decodedCursor=target; value.readyGeneration.store(voice->generation.load());
        system_->Refresh(*voice);
    }
    system_->changed.notify_all();
    rollback.committed=true;
    return handle;
}
void AudioSystem::SetMasterVolume(float volume) {
    detail::GainValue(volume); std::lock_guard lock(system_->control); system_->master=volume;
    for(const auto& voice:system_->active) system_->ApplyGain(*voice);
}
float AudioSystem::GetMasterVolume() const { std::lock_guard lock(system_->control); return system_->master; }
void AudioSystem::SetBusVolume(const std::string& bus,double volume) {
    detail::Name(bus,"audio bus"); detail::GainValue(volume);
    std::lock_guard lock(system_->control); system_->buses[bus]=volume;
    for(const auto& voice:system_->active) if(voice->bus==bus) system_->ApplyGain(*voice);
}
double AudioSystem::GetBusVolume(const std::string& bus) const { detail::Name(bus,"audio bus"); std::lock_guard lock(system_->control); return system_->BusGain(bus); }
void AudioSystem::Update() {
    std::lock_guard lock(system_->control);
    if(!system_->alive) return;
    for(const auto& voice:system_->active) system_->Refresh(*voice);
    system_->Reclaim();
}
void AudioSystem::CheckErrors() {
    if(std::this_thread::get_id()!=system_->owner)
        throw std::logic_error("AudioSystem::CheckErrors must run on the owner thread");
    std::string message;
    {
        std::lock_guard lock(system_->control);
        if(system_->errorCount) {
            message=system_->errors[system_->errorHead].data();
            system_->errorHead=(system_->errorHead+1)%system_->errors.size();
            --system_->errorCount;
        } else if(system_->droppedErrors) {
            message="audio error queue overflow: "+std::to_string(system_->droppedErrors)+" additional errors were suppressed";
            system_->droppedErrors=0;
        }
    }
    if(!message.empty()) throw std::runtime_error(message);
}
std::size_t AudioSystem::GetActivePlaybackCount() const {
    std::lock_guard lock(system_->control);
    return static_cast<std::size_t>(std::count_if(system_->active.begin(),system_->active.end(),[](const auto& voice){return !detail::Terminal(voice->state.load());}));
}
namespace testing {
std::uint32_t Access::DeviceId(const AudioSystem& audio) {
    std::lock_guard lock(audio.system_->control); return audio.system_->device;
}
bool Access::HasDecoderWorker(const AudioSystem& audio) {
    std::lock_guard lock(audio.system_->control); return audio.system_->worker.joinable();
}
void Access::PauseDecoder(AudioSystem& audio,bool pause) {
    std::lock_guard lock(audio.system_->control); audio.system_->decoderPaused=pause; audio.system_->changed.notify_all();
}
Snapshot Access::Inspect(const Playback& playback) {
    detail::LockedVoice locked(*playback.voice_);
    Snapshot result;
    result.state=playback.voice_->state.load();
    result.generation=playback.voice_->generation.load();
    result.submittedFrame=playback.voice_->submitted.load();
    result.hasTransport=playback.voice_->transport!=nullptr;
    if(const auto* value=playback.voice_->transport.get()) {
        const auto write=value->write.load(std::memory_order_acquire);
        const auto read=value->read.load(std::memory_order_acquire);
        result.prefetchedFrames=write>=read?static_cast<std::size_t>(write-read):0;
        if(value->stream) {
            result.effectiveGain=SDL_GetAudioStreamGain(value->stream);
            result.streamQueuedBytes=SDL_GetAudioStreamQueued(value->stream);
            result.streamAvailableBytes=SDL_GetAudioStreamAvailable(value->stream);
        }
    }
    return result;
}
}
}
