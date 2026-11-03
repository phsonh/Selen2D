#pragma once
#include "Compiler.h"
#include <SDL3/SDL_gpu.h>
#include <cstddef>
#include <memory>

namespace core::modules::Graphic::Shader {
namespace detail { struct ProgramState; }

// Native-only immutable shader program. Access and final release while the
// device is live belong to the render owner thread. Store shutdown invalidates
// retained programs and releases their GPU objects before device destruction.
class Program final {
public:
    ~Program();
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    SDL_GPUShader* Vertex() const;
    SDL_GPUShader* Fragment() const;
    const Compiled& Metadata() const;
private:
    explicit Program(std::shared_ptr<detail::ProgramState>);
    std::shared_ptr<detail::ProgramState> state_;
    friend class ProgramStore;
};

class ProgramStore final {
public:
    explicit ProgramStore(SDL_GPUDevice*, std::size_t capacity = 1024);
    ~ProgramStore();
    ProgramStore(const ProgramStore&) = delete;
    ProgramStore& operator=(const ProgramStore&) = delete;
    std::shared_ptr<const Program> Create(std::shared_ptr<const Compiled>);
    void Shutdown() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
