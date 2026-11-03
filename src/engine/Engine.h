#pragma once
#include <string>
#include <cstdint>
#include <memory>

namespace core::modules::File { class FileSystem; }

namespace engine
{
    bool Init(const std::string& entry, std::shared_ptr<const core::modules::File::FileSystem> files);
    bool Run(std::uint64_t frame_limit = 0);
    bool Shutdown();
}
