#include "ResourceHost.h"
#include "core/modules/Resource/ImageProvider.h"
#include "core/modules/Resource/CpuProvider.h"
#include "core/modules/Resource/ShaderProvider.h"
#include <SDL3/SDL_timer.h>
#include <stdexcept>
#include <thread>

namespace engine {
namespace R = core::modules::Resource;
namespace J = core::runtime::Jobs;
namespace U = core::modules::Graphic::Upload;
namespace {
constexpr std::size_t MiB = 1024 * 1024;
class Providers final : public R::Provider {
public:
    R::Images::Provider images;
    R::CpuProvider cpu;
    std::unique_ptr<R::ShaderProvider> shader;
    Providers(J::Executor& workers,R::ImageSources& sources,U::Queue& uploads,J::MemoryBudget pages,
        std::shared_ptr<const core::modules::File::FileSystem> files,J::MemoryBudget native,
        core::modules::Audio::AudioSystem* audio,core::modules::Graphic::Shader::ProgramStore* programs)
        :images(workers,sources,uploads,std::move(pages)),cpu(workers,files,native,audio) {
        if(programs) shader=std::make_unique<R::ShaderProvider>(workers,std::move(files),std::move(native),*programs);
    }
    std::unique_ptr<R::Operation> Start(R::Handle handle,const R::Request& request) override {
        if(request.id.kind==R::Kind::Shader) {
            if(!shader) throw std::runtime_error("Shader GPU service is unavailable");
            return shader->Start(handle,request);
        }
        return request.id.kind==R::Kind::Image ? images.Start(handle,request) : cpu.Start(handle,request);
    }
    void Shutdown() noexcept override { images.Shutdown(); cpu.Shutdown(); if(shader) shader->Shutdown(); }
};
}

struct ResourceHost::Impl {
    const std::thread::id owner{std::this_thread::get_id()};
    J::MemoryBudget sourceMemory{256 * MiB};
    J::MemoryBudget pageMemory{128 * MiB};
    J::MemoryBudget nativeMemory{256 * MiB};
    U::Queue& uploads;
    J::Executor workers;
    R::ImageSources sources;
    R::Images::Provider* images{}; // Owned by service; never exposed to Lua.
    std::unique_ptr<R::Service> service;
    bool closed{};

    Impl(U::Queue& queue, std::shared_ptr<const core::modules::File::FileSystem> files,
        core::modules::Audio::AudioSystem* audio,core::modules::Graphic::Shader::ProgramStore* programs)
        :uploads(queue), sources(workers, sourceMemory, R::SourceConfig{.files=files}) {
        auto provider = std::make_unique<Providers>(workers,sources,uploads,pageMemory,std::move(files),nativeMemory,audio,programs);
        images = &provider->images;
        service = std::make_unique<R::Service>(std::move(provider));
    }
    void Running() const {
        if (owner != std::this_thread::get_id())
            throw std::logic_error("ResourceHost requires its owner thread");
        if (closed) throw std::logic_error("ResourceHost has been shut down");
    }
    void Shutdown() noexcept {
        if (owner != std::this_thread::get_id()) std::terminate();
        if (closed) return;
        closed = true;
        // Closing the registry terminates Jobs and detaches provider work.
        // Workers own detached inputs/results, not this host or the Lua VM.
        // Only after cancellation may we close source admission and join them.
        service->Shutdown();
        sources.Shutdown();
        workers.Shutdown();
        // Upload fences/physical texture retirement remain Graphic-owned.
        // Graphic::Shutdown drains them before destroying the SDL_GPU device.
    }
};

ResourceHost::ResourceHost(U::Queue& uploads, std::shared_ptr<const core::modules::File::FileSystem> files,
    core::modules::Audio::AudioSystem* audio,core::modules::Graphic::Shader::ProgramStore* programs)
    : impl(std::make_unique<Impl>(uploads,std::move(files),audio,programs)) {}
ResourceHost::~ResourceHost() { Shutdown(); }
R::Service& ResourceHost::Resources() { impl->Running(); return *impl->service; }
void ResourceHost::Tick() {
    impl->Running();
    impl->service->Tick();
    impl->images->Tick();
}
void ResourceHost::Shutdown() noexcept { if (impl) impl->Shutdown(); }
void ResourceHost::Wait(R::Job& job) {
    impl->Running();
    while(!job.IsFinished()) {
        Tick();
        impl->uploads.Tick();
        if(!impl->uploads.FatalError().empty()) throw std::runtime_error(impl->uploads.FatalError());
        if(!job.IsFinished()) SDL_Delay(1);
    }
    job.CheckResult();
}
}
