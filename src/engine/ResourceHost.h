#pragma once
#include <memory>

namespace core::modules::Graphic::Upload { class Queue; }
namespace core::modules::Graphic::Shader { class ProgramStore; }
namespace core::modules::Resource { class Service; class Job; }
namespace core::modules::Audio { class AudioSystem; }
namespace core::modules::File { class FileSystem; }

namespace engine {
// Composition and shutdown order belong to the application host. Neither the
// resource registry nor Graphic acquires a dependency on Object or the Lua VM.
class ResourceHost final {
public:
    // The Graphic-owned upload queue must outlive this host.
    ResourceHost(core::modules::Graphic::Upload::Queue&, std::shared_ptr<const core::modules::File::FileSystem>,
        core::modules::Audio::AudioSystem* = nullptr, core::modules::Graphic::Shader::ProgramStore* = nullptr);
    ~ResourceHost();
    ResourceHost(const ResourceHost&) = delete;
    ResourceHost& operator=(const ResourceHost&) = delete;

    core::modules::Resource::Service& Resources();
    void Tick(); // Once per host frame, independently of Lua/Object updates.
    void Wait(core::modules::Resource::Job&); // Sync loads only; no Lua/Object/Renderer reentry.
    void Shutdown() noexcept; // Cancel, close source cache, then join CPU workers.
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
