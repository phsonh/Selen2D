#pragma once
#include "Resource.h"
#include "core/runtime/Jobs/Executor.h"
namespace core::modules::File { class FileSystem; }
namespace core::modules::Audio { class AudioSystem; }
namespace core::modules::Resource {
// Audio/font preparation shares the CPU scheduler, not the Image/Atlas system.
class CpuProvider final : public Provider {
public:
    CpuProvider(runtime::Jobs::Executor&, std::shared_ptr<const File::FileSystem>,
        runtime::Jobs::MemoryBudget, Audio::AudioSystem*);
    ~CpuProvider();
    std::unique_ptr<Operation> Start(Handle,const Request&) override;
    void Shutdown() noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
