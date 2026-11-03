#include "Decoder.h"
#include "core/modules/File/Vfs.h"
#include <SDL3/SDL.h>
#include <vorbis/vorbisfile.h>
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include <dr_wav.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace core::modules::Audio::detail {
namespace {
struct CloseIO { void operator()(SDL_IOStream* value) const { if (value) SDL_CloseIO(value); } };
using IO = std::unique_ptr<SDL_IOStream, CloseIO>;
IO Open(const File::FileSystem& files, const std::string& path) {
    IO value(files.Open(path, "rb"));
    if (!value) throw std::runtime_error("cannot open audio '" + path + "': " + SDL_GetError());
    return value;
}
void Validate(const ResourceInfo& info) {
    if ((info.channels != 1 && info.channels != 2) || info.sampleRate <= 0 || !info.frames)
        throw std::runtime_error("audio must contain nonempty mono/stereo PCM");
}
class Wav final : public Decoder {
public:
    Wav(IO io, const std::string& path) : io_(std::move(io)) {
        if (!drwav_init(&wav_, ReadBytes, SeekBytes, TellBytes, io_.get(), nullptr))
            throw std::runtime_error("invalid WAV audio: " + path);
        opened_=true;
        info_.sampleRate=static_cast<int>(wav_.sampleRate);
        info_.channels=static_cast<int>(wav_.channels);
        info_.frames=wav_.totalPCMFrameCount;
        try { Validate(info_); } catch (...) { drwav_uninit(&wav_); opened_=false; throw; }
    }
    ~Wav() override { if(opened_) drwav_uninit(&wav_); }
    ResourceInfo GetInfo() const override { return info_; }
    void Seek(std::uint64_t frame) override {
        if (frame > info_.frames || !drwav_seek_to_pcm_frame(&wav_, frame))
            throw std::runtime_error("WAV PCM seek failed");
    }
    std::size_t Read(std::span<float> destination) override {
        const auto frames=static_cast<std::size_t>(drwav_read_pcm_frames_f32(&wav_, destination.size()/2, destination.data()));
        if(SDL_GetIOStatus(io_.get())==SDL_IO_STATUS_ERROR) throw std::runtime_error("WAV source read failed");
        if(info_.channels==1) for(auto i=frames; i>0; --i) destination[2*i-1]=destination[2*i-2]=destination[i-1];
        return frames;
    }
private:
    static std::size_t ReadBytes(void* io, void* target, std::size_t bytes) { return SDL_ReadIO(static_cast<SDL_IOStream*>(io),target,bytes); }
    static drwav_bool32 SeekBytes(void* io,int offset,drwav_seek_origin origin) {
        const auto whence=origin==DRWAV_SEEK_SET ? SDL_IO_SEEK_SET : origin==DRWAV_SEEK_END ? SDL_IO_SEEK_END : SDL_IO_SEEK_CUR;
        return SDL_SeekIO(static_cast<SDL_IOStream*>(io),offset,whence)>=0;
    }
    static drwav_bool32 TellBytes(void* io,drwav_int64* position) { *position=SDL_TellIO(static_cast<SDL_IOStream*>(io)); return *position>=0; }
    IO io_;
    drwav wav_{};
    bool opened_{};
    ResourceInfo info_{};
};
class Vorbis final : public Decoder {
public:
    Vorbis(IO io, const std::string& path) : io_(std::move(io)) {
        const ov_callbacks callbacks{ReadBytes,SeekBytes,nullptr,TellBytes};
        if(ov_open_callbacks(io_.get(),&file_,nullptr,0,callbacks)!=0) throw std::runtime_error("invalid Vorbis audio: "+path);
        opened_=true;
        try {
            const auto* value=ov_info(&file_,0);
            const auto frames=ov_pcm_total(&file_,-1);
            if(!value || frames<=0 || value->rate>std::numeric_limits<int>::max()) throw std::runtime_error("invalid Vorbis PCM format");
            info_.channels=value->channels; info_.sampleRate=static_cast<int>(value->rate); info_.frames=static_cast<std::uint64_t>(frames);
            Validate(info_);
            for(long i=1; i<ov_streams(&file_); ++i) {
                value=ov_info(&file_,static_cast<int>(i));
                if(!value || value->channels!=info_.channels || value->rate!=info_.sampleRate)
                    throw std::runtime_error("Vorbis chained streams must share one PCM format");
            }
        } catch(...) { ov_clear(&file_); opened_=false; throw; }
    }
    ~Vorbis() override { if(opened_) ov_clear(&file_); }
    ResourceInfo GetInfo() const override { return info_; }
    void Seek(std::uint64_t frame) override {
        if(frame>info_.frames || ov_pcm_seek(&file_,static_cast<ogg_int64_t>(frame))!=0) throw std::runtime_error("Vorbis PCM seek failed");
    }
    std::size_t Read(std::span<float> destination) override {
        std::size_t total{};
        while(total<destination.size()/2) {
            float** planes{}; int section{};
            const auto requested=static_cast<int>(std::min<std::size_t>(destination.size()/2-total,65536));
            const auto actual=ov_read_float(&file_,&planes,requested,&section);
            if(SDL_GetIOStatus(io_.get())==SDL_IO_STATUS_ERROR) throw std::runtime_error("Vorbis source read failed");
            if(actual<0) throw std::runtime_error("Vorbis PCM decode failed");
            if(!actual) break;
            for(long i=0;i<actual;++i) { destination[2*(total+i)]=planes[0][i]; destination[2*(total+i)+1]=planes[info_.channels==1 ? 0 : 1][i]; }
            total+=static_cast<std::size_t>(actual);
        }
        return total;
    }
private:
    static std::size_t ReadBytes(void* target,std::size_t size,std::size_t count,void* io) {
        if(!size || count>std::numeric_limits<std::size_t>::max()/size) return 0;
        return SDL_ReadIO(static_cast<SDL_IOStream*>(io),target,size*count)/size;
    }
    static int SeekBytes(void* io,ogg_int64_t offset,int whence) {
        const auto origin=whence==SEEK_SET ? SDL_IO_SEEK_SET : whence==SEEK_END ? SDL_IO_SEEK_END : SDL_IO_SEEK_CUR;
        return SDL_SeekIO(static_cast<SDL_IOStream*>(io),offset,origin)>=0 ? 0 : -1;
    }
    static long TellBytes(void* io) {
        const auto result=SDL_TellIO(static_cast<SDL_IOStream*>(io));
        return result>=0 && result<=std::numeric_limits<long>::max() ? static_cast<long>(result) : -1;
    }
    IO io_;
    OggVorbis_File file_{};
    bool opened_{};
    ResourceInfo info_{};
};
}
std::unique_ptr<Decoder> OpenDecoder(std::shared_ptr<const File::FileSystem> files, const std::string& path) {
    if(!files) throw std::invalid_argument("audio requires a virtual file system");
    const auto normalized=files->NormalizePath(path);
    if(!normalized || normalized->empty()) throw std::invalid_argument("invalid virtual audio path");
    auto io=Open(*files,*normalized);
    std::array<char,12> magic{};
    const auto count=SDL_ReadIO(io.get(),magic.data(),magic.size());
    if(SDL_GetIOStatus(io.get())==SDL_IO_STATUS_ERROR) throw std::runtime_error("audio header read failed: "+path);
    if(SDL_SeekIO(io.get(),0,SDL_IO_SEEK_SET)<0) throw std::runtime_error("audio source rewind failed: "+path);
    if(count>=4 && std::memcmp(magic.data(),"OggS",4)==0) return std::make_unique<Vorbis>(std::move(io),*normalized);
    if(count==12 && (std::memcmp(magic.data(),"RIFF",4)==0 || std::memcmp(magic.data(),"RF64",4)==0)
        && std::memcmp(magic.data()+8,"WAVE",4)==0) {
        // dr_wav tolerates truncated RIFF data by reducing the frame count.
        // Do not silently publish that shortened asset as a complete resource.
        if(std::memcmp(magic.data(),"RIFF",4)==0) {
            std::uint64_t declared{};
            for(unsigned i=0;i<4;++i) declared|=std::uint64_t(static_cast<unsigned char>(magic[4+i]))<<(8*i);
            const auto size=SDL_GetIOSize(io.get());
            if(size<0 || declared+8>static_cast<std::uint64_t>(size))
                throw std::runtime_error("truncated WAV audio: "+path);
        }
        return std::make_unique<Wav>(std::move(io),*normalized);
    }
    throw std::runtime_error("audio file is neither supported WAV nor Ogg Vorbis: "+path);
}
std::shared_ptr<const ResourceData> PrepareResource(std::shared_ptr<const File::FileSystem> files,
    const std::string& path,const LoadOptions& options) {
    // Synchronous/native callers share a finite allowance too. Keeping several
    // prepared sounds cannot bypass the loader's explicit bounded path.
    static core::runtime::Jobs::MemoryBudget memory(256u*1024u*1024u);
    return PrepareResource(std::move(files),path,options,memory,{});
}
std::shared_ptr<const ResourceData> PrepareResource(std::shared_ptr<const File::FileSystem> files,
    const std::string& path,const LoadOptions& options,
    core::runtime::Jobs::MemoryBudget memory,const core::runtime::Jobs::StopToken& stop) {
    stop.Check();
    if(path.empty()) throw std::invalid_argument("audio path cannot be empty");
    if(options.mode!=StorageMode::Decoded && options.mode!=StorageMode::Stream) throw std::invalid_argument("invalid audio storage mode");
    auto decoder=OpenDecoder(files,path);
    stop.Check();
    auto result=std::make_shared<ResourceData>();
    result->info=decoder->GetInfo();
    result->info.mode=options.mode;
    result->info.duration=static_cast<double>(result->info.frames)/result->info.sampleRate;
    result->info.loopEndFrame=result->info.frames;
    result->path=*files->NormalizePath(path);
    result->files=std::move(files);
    if(options.loop) {
        const auto begin=options.loop->start;
        const auto end=options.loop->finish.value_or(result->info.duration);
        if(!std::isfinite(begin)||!std::isfinite(end)||begin<0||end<=begin||end>result->info.duration)
            throw std::invalid_argument("audio loop seconds must satisfy 0 <= start < finish <= duration");
        result->info.loopStartFrame=static_cast<std::uint64_t>(std::round(begin*result->info.sampleRate));
        result->info.loopEndFrame=static_cast<std::uint64_t>(std::round(end*result->info.sampleRate));
        if(result->info.loopStartFrame>=result->info.loopEndFrame) throw std::invalid_argument("audio loop collapses to zero PCM frames");
    }
    if(options.mode==StorageMode::Decoded) {
        if(result->info.frames>std::numeric_limits<std::size_t>::max()/(2*sizeof(float))) throw std::runtime_error("decoded audio exceeds addressable memory");
        const auto bytes=static_cast<std::size_t>(result->info.frames)*2*sizeof(float);
        constexpr std::size_t MaximumPcmBytes=128u*1024u*1024u;
        if(bytes>MaximumPcmBytes) throw std::length_error("decoded audio PCM exceeds the 128 MiB per-resource limit: "+path);
        auto credit=memory.TryAcquire(bytes,stop);
        if(!credit) throw std::runtime_error("audio PCM budget exhausted; unload resources before retrying: "+path);
        result->credit=std::move(*credit);
        stop.Check();
        result->pcm.resize(static_cast<std::size_t>(result->info.frames)*2);
        std::size_t offset{};
        while(offset<result->info.frames) {
            stop.Check();
            const auto requested=static_cast<std::size_t>(std::min<std::uint64_t>(8192,result->info.frames-offset));
            const auto count=decoder->Read(std::span(result->pcm).subspan(offset*2,requested*2));
            stop.Check();
            if(!count) throw std::runtime_error("audio decoder ended before declared frame count");
            offset+=count;
        }
    }
    stop.Check();
    return result;
}
}
