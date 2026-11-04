#pragma once
#include <filesystem>
#include <string>
#include <memory>
#include "LaunchConfig.h"

namespace core::modules::File { class FileSystem; }

namespace engine::Startup {
struct Config {
    // Executable directory; Lua entry paths never change this root.
    std::filesystem::path directory;
    std::string entry;
    std::shared_ptr<const core::modules::File::FileSystem> files;
};
// Uses only the compiled launch.h. Never reads an external configuration or
// falls back to one. Does not execute Lua or change the process working directory.
Config Load(const std::filesystem::path& executableDirectory);
// Native validation seam, also used by the compiled configuration above.
Config Load(const std::filesystem::path& executableDirectory, const launch::Config& declaration);
}
