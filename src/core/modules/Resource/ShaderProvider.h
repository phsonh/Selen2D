#pragma once
#include "Resource.h"
#include "core/modules/Graphic/shader/Program.h"
#include "core/runtime/Jobs/Executor.h"
#include <utility>

namespace core::modules::File { class FileSystem; }
namespace core::modules::Resource {
struct ShaderAsset final : Asset {
    explicit ShaderAsset(std::shared_ptr<const Graphic::Shader::Program> value) : program(std::move(value)) {}
    std::shared_ptr<const Graphic::Shader::Program> program;
};

// Worker compilation is independent from owner-thread GPU creation. The
// provider cancels work at shutdown but never shuts down Graphic's program store.
class ShaderProvider final : public Provider {
public:
    ShaderProvider(runtime::Jobs::Executor&, std::shared_ptr<const File::FileSystem>,
        runtime::Jobs::MemoryBudget, Graphic::Shader::ProgramStore&);
    ~ShaderProvider();
    std::unique_ptr<Operation> Start(Handle, const Request&) override;
    void Shutdown() noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
