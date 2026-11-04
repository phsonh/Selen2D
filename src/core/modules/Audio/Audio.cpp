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
#include <limits>
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
void PanValue(double value) {
    if(!std::isfinite(value)||value < -1||value>1) throw std::invalid_argument("audio pan must be finite and between -1 and 1");
}
void Name(const std::string& value,const char* kind) {
    if(value.empty()||value.find('\0')!=std::string::npos) throw std::invalid_argument(std::string(kind)+" must be a nonempty name without NUL");
}
bool Terminal(PlaybackState value) {
    return value==PlaybackState::Stopped||value==PlaybackState::Finished||value==PlaybackState::Failed||value==PlaybackState::Destroyed;
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
    std::string error;
    std::uint64_t target{};
    std::uint64_t identity{};
    bool pausedIntent{};
    std::unique_ptr<Transport> transport;
};
struct PlayerEntry {
    std::string name;
    std::shared_ptr<Voice> voice;
    std::optional<std::uint64_t> playedFrame;
};
struct PlayerState {
    std::weak_ptr<System> system;
    std::atomic<bool> destroyed{};
    double volume{1},pan{}; // System control lock, preserve the Lua number.
    std::unordered_map<std::uint64_t,PlayerEntry> entries;
};
struct System {
    const std::thread::id owner{std::this_thread::get_id()};
    mutable std::mutex control;
    std::condition_variable changed;
    SDL_AudioDeviceID device{};
    SDL_AudioSpec output{};
    bool alive{},exit{},decoderPaused{};
    std::uint64_t nextResourceGeneration{1},nextVoiceIdentity{1},runtimeFrame{};
    std::vector<std::weak_ptr<PlayerState>> players;
    std::vector<std::shared_ptr<Voice>> active;
    std::thread worker;
    std::size_t nextVoice{};
    // Fixed storage: reporting failure on the service worker must not itself
    // allocate or grow an unbounded diagnostics queue. Control lock protects it.
    std::array<std::array<char,2048>,32> errors{};
    std::size_t errorHead{},errorCount{},droppedErrors{};

    void RequireAlive() const { if(!alive||exit) throw std::runtime_error("audio subsystem is shut down"); }
    void ApplyGain(Voice& voice) {
        if(voice.transport&&voice.transport->stream)
            Check(SDL_SetAudioStreamGain(voice.transport->stream,static_cast<float>(voice.volume.load()
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
        try { Invalidate(voice); }
        catch(...) { voice.state.store(state,std::memory_order_release); changed.notify_all(); throw; }
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
        Complete(voice);
        if(!voice.transport||Terminal(voice.state.load())) return;
        auto& value=*voice.transport;
        const auto generation=voice.generation.load(std::memory_order_acquire);
        if(value.readyGeneration.load(std::memory_order_acquire)==generation&&value.appliedGeneration!=generation) {
            if(voice.state.load()==PlaybackState::Starting||voice.state.load()==PlaybackState::Paused) {
                voice.submitted.store(voice.target,std::memory_order_release);
                value.appliedGeneration=generation;
                if(voice.pausedIntent) voice.state.store(PlaybackState::Paused);
                else {
                    voice.state.store(PlaybackState::Playing,std::memory_order_release);
                    if(!value.bound) {
                        if(!SDL_BindAudioStream(device,value.stream)) { Fail(voice,std::string("bind audio playback: ")+SDL_GetError()); return; }
                        value.bound=true;
                    }
                }
            }
        }
    }
    // Queries may observe natural completion/faults, but must never prime,
    // bind, resume, decode, or wait for an entire sound to finish.
    void Complete(Voice& voice) {
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
        for(auto it=players.begin();it!=players.end();) {
            if(auto player=it->lock()) {
                for(auto& [generation,entry]:player->entries)
                    if(entry.voice&&Terminal(entry.voice->state.load())) entry.voice.reset();
                ++it;
            } else it=players.erase(it);
        }
    }
    void RetirePlayers(std::uint64_t generation) {
        for(const auto& weak:players) if(auto player=weak.lock()) player->entries.erase(generation);
    }
    void DestroyPlayer(PlayerState& player) noexcept {
        player.destroyed.store(true,std::memory_order_release);
        for(auto& [generation,entry]:player.entries) if(entry.voice) {
            try { Stop(*entry.voice); }
            catch(const std::exception& error) { Report(entry.voice.get(),error.what()); }
            catch(...) { Report(entry.voice.get(),"unknown error while destroying audio player"); }
        }
        player.entries.clear();
        changed.notify_all();
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
struct LockedPlayer {
    std::shared_ptr<System> system;
    std::unique_lock<std::mutex> lock;
    explicit LockedPlayer(const PlayerState& player)
        : system(player.system.lock()),lock(system?std::unique_lock(system->control):std::unique_lock<std::mutex>()) {
        if(player.destroyed.load()) throw std::runtime_error("audio player has been destroyed");
        if(!system) throw std::runtime_error("audio player service no longer exists");
        system->RequireAlive();
    }
};
void CheckRecord(const std::shared_ptr<System>& system,const std::shared_ptr<Record>& record) {
    if(!record||record->system.lock()!=system) throw std::invalid_argument("audio source belongs to another service");
    if(record->retired.load(std::memory_order_acquire)) throw std::runtime_error("audio source has been retired");
}
bool Playing(System& system,const std::shared_ptr<Voice>& voice) {
    if(!voice) return false;
    system.Complete(*voice);
    const auto state=voice->state.load(std::memory_order_acquire);
    return state==PlaybackState::Playing||state==PlaybackState::Starting;
}
std::shared_ptr<Voice> CreateVoice(const std::shared_ptr<System>& system,const std::shared_ptr<Record>& record,
    double volume,double pan) {
    auto voice=std::make_shared<Voice>();
    if(system->nextVoiceIdentity==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("audio instance identity exhausted");
    voice->identity=system->nextVoiceIdentity++;
    voice->system=system; voice->info=record->data->info;
    voice->volume.store(static_cast<float>(volume)); voice->pan.store(static_cast<float>(pan));
    voice->transport=std::make_unique<Transport>(record,voice->info.mode==StorageMode::Stream);
    auto& value=*voice->transport;
    value.owner=voice.get();
    const SDL_AudioSpec sourceFormat{SDL_AUDIO_F32,2,voice->info.sampleRate};
    value.stream=SDL_CreateAudioStream(&sourceFormat,&system->output);
    Check(value.stream!=nullptr,"create audio player stream");
    struct Rollback {
        System& system;
        const std::shared_ptr<Voice>& voice;
        bool committed{};
        ~Rollback() noexcept {
            if(committed) return;
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
    } rollback{*system,voice};
    Check(SDL_SetAudioStreamGetCallback(value.stream,Supply,&value),"attach audio callback");
    system->ApplyGain(*voice);
    system->active.push_back(voice);
    if(voice->info.mode==StorageMode::Decoded) {
        const auto generation=voice->generation.load();
        value.readyGeneration.store(generation);
        value.appliedGeneration=generation;
        voice->state.store(PlaybackState::Playing,std::memory_order_release);
        // Initial setup is a synchronous Play error. Do not also enqueue it as
        // a later uncaught asynchronous failure after Lua handled this error.
        Check(SDL_BindAudioStream(system->device,value.stream),"start audio player");
        value.bound=true;
    }
    system->changed.notify_all();
    rollback.committed=true;
    return voice;
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
                    auto decoder=OpenDecoder(data->source,data->path);
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
        system->RetirePlayers(record_->generation);
        system->changed.notify_all();
    } catch(...) { /* Retired is already visible to the callback/worker. */ }
}
Player::Player(std::shared_ptr<detail::PlayerState> state) : state_(std::move(state)) {}
Player::~Player() { Destroy(); }
void Player::Play(const std::string& name,const std::shared_ptr<Source>& source) {
    detail::Name(name,"audio resource");
    detail::LockedPlayer locked(*state_);
    if(!source) throw std::invalid_argument("audio source cannot be null");
    detail::CheckRecord(locked.system,source->record_);
    auto& entry=state_->entries.try_emplace(source->record_->generation,detail::PlayerEntry{name}).first->second;
    if(entry.playedFrame==locked.system->runtimeFrame) return;
    if(entry.voice) locked.system->Stop(*entry.voice);
    entry.voice.reset();
    entry.voice=detail::CreateVoice(locked.system,source->record_,state_->volume,state_->pan);
    entry.playedFrame=locked.system->runtimeFrame;
}
void Player::Stop(const std::shared_ptr<Source>& source) {
    detail::LockedPlayer locked(*state_);
    if(source) {
        detail::CheckRecord(locked.system,source->record_);
        const auto it=state_->entries.find(source->record_->generation);
        if(it==state_->entries.end()) return;
        auto voice=it->second.voice;
        state_->entries.erase(it); // Even a fallible device clear revokes the marker.
        if(voice) locked.system->Stop(*voice);
    } else {
        std::exception_ptr failure;
        for(auto& [generation,entry]:state_->entries) if(entry.voice) {
            try { locked.system->Stop(*entry.voice); }
            catch(...) { if(!failure) failure=std::current_exception(); }
        }
        state_->entries.clear();
        if(failure) std::rethrow_exception(failure);
    }
}
void Player::Pause(const std::shared_ptr<Source>& source) {
    detail::LockedPlayer locked(*state_);
    if(source) detail::CheckRecord(locked.system,source->record_);
    for(auto& [generation,entry]:state_->entries)
        if((!source||source->record_->generation==generation)&&entry.voice) locked.system->Pause(*entry.voice);
}
void Player::Resume(const std::shared_ptr<Source>& source) {
    detail::LockedPlayer locked(*state_);
    if(source) detail::CheckRecord(locked.system,source->record_);
    for(auto& [generation,entry]:state_->entries)
        if((!source||source->record_->generation==generation)&&entry.voice) locked.system->Resume(*entry.voice);
}
void Player::SetVolume(double volume) {
    detail::GainValue(volume);
    detail::LockedPlayer locked(*state_);
    state_->volume=volume;
    for(auto& [generation,entry]:state_->entries) if(entry.voice) {
        entry.voice->volume.store(static_cast<float>(volume));
        locked.system->ApplyGain(*entry.voice);
    }
}
double Player::GetVolume() const { detail::LockedPlayer locked(*state_); return state_->volume; }
void Player::SetPan(double pan) {
    detail::PanValue(pan);
    detail::LockedPlayer locked(*state_);
    state_->pan=pan;
    for(auto& [generation,entry]:state_->entries) if(entry.voice) entry.voice->pan.store(static_cast<float>(pan));
}
double Player::GetPan() const { detail::LockedPlayer locked(*state_); return state_->pan; }
std::vector<std::string> Player::GetPlaying() const {
    detail::LockedPlayer locked(*state_);
    std::vector<std::string> names;
    for(const auto& [generation,entry]:state_->entries)
        if(detail::Playing(*locked.system,entry.voice)) names.push_back(entry.name);
    std::sort(names.begin(),names.end());
    names.erase(std::unique(names.begin(),names.end()),names.end());
    return names;
}
bool Player::IsPlaying(const std::shared_ptr<Source>& source) const {
    detail::LockedPlayer locked(*state_);
    if(!source) throw std::invalid_argument("audio source cannot be null");
    detail::CheckRecord(locked.system,source->record_);
    const auto it=state_->entries.find(source->record_->generation);
    return it!=state_->entries.end()&&detail::Playing(*locked.system,it->second.voice);
}
void Player::Destroy() noexcept {
    if(state_->destroyed.load()) return;
    try {
        if(auto system=state_->system.lock()) {
            std::lock_guard lock(system->control);
            system->DestroyPlayer(*state_);
        } else state_->destroyed.store(true,std::memory_order_release);
    } catch(...) { state_->destroyed.store(true,std::memory_order_release); }
}
bool Player::IsDestroyed() const noexcept { return state_->destroyed.load(std::memory_order_acquire); }

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
    StopAllPlayersAndDrain();
    {
        std::lock_guard lock(system->control);
        if(!system->alive) { files_.reset(); return; }
        system->alive=false;
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
    if(system_->nextResourceGeneration==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("audio resource generation exhausted");
    record->generation=system_->nextResourceGeneration++;
    return std::shared_ptr<Source>(new Source(std::move(record)));
}
std::shared_ptr<Player> AudioSystem::CreatePlayer() {
    auto state=std::make_shared<detail::PlayerState>();
    auto result=std::shared_ptr<Player>(new Player(state));
    std::lock_guard lock(system_->control); system_->RequireAlive();
    state->system=system_;
    system_->players.emplace_back(state);
    return result;
}
void AudioSystem::SetRuntimeFrame(std::uint64_t frame) {
    std::lock_guard lock(system_->control); system_->RequireAlive();
    if(frame<system_->runtimeFrame) throw std::logic_error("audio Runtime frame cannot move backwards");
    system_->runtimeFrame=frame;
}
void AudioSystem::StopAllPlayersAndDrain() {
    auto system=system_;
    {
        std::lock_guard lock(system->control);
        if(!system->alive) return;
        system->exit=true;
        for(const auto& weak:system->players) if(auto player=weak.lock()) system->DestroyPlayer(*player);
        for(const auto& voice:system->active) {
            try { system->Stop(*voice); }
            catch(const std::exception& error) { system->Report(voice.get(),error.what()); }
            catch(...) { system->Report(voice.get(),"unknown error while stopping audio consumers"); }
        }
    }
    system->changed.notify_all();
    if(system->worker.joinable()) system->worker.join();
    std::lock_guard lock(system->control);
    system->Reclaim();
}
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
Snapshot Access::Inspect(const Player& player,const Source& source) {
    auto system=player.state_->system.lock();
    if(!system) return {};
    std::lock_guard lock(system->control);
    Snapshot result;
    const auto it=player.state_->entries.find(source.record_->generation);
    if(it==player.state_->entries.end()) return result;
    result.hasEntry=true;
    result.hasPlayedFrame=it->second.playedFrame.has_value();
    result.playedFrame=it->second.playedFrame.value_or(0);
    if(!it->second.voice) { result.state=PlaybackState::Finished; return result; }
    const auto& voice=*it->second.voice;
    result.state=voice.state.load();
    result.instance=voice.identity;
    result.generation=voice.generation.load();
    result.submittedFrame=voice.submitted.load();
    result.hasTransport=voice.transport!=nullptr;
    if(const auto* value=voice.transport.get()) {
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
std::size_t Access::PlayerCount(const AudioSystem& audio) {
    std::lock_guard lock(audio.system_->control);
    return static_cast<std::size_t>(std::count_if(audio.system_->players.begin(),audio.system_->players.end(),[](const auto& weak) {
        const auto player=weak.lock(); return player&&!player->destroyed.load();
    }));
}
}
}
