#pragma once
#include <filesystem>
#include <string>
#include <memory>

namespace core::modules::File { class FileSystem; }

namespace engine::Startup {
struct Config {
    // Executable directory; Lua entry paths never change this root.
    std::filesystem::path directory;
    std::string entry;
    std::shared_ptr<const core::modules::File::FileSystem> files;
};
// Use launch.lua in the executable directory with ordinary root-relative files.
// Never reads an external manifest or falls back to one. Throws if the root or
// entry is invalid; does not execute Lua or mutate the working directory.
Config Load(const std::filesystem::path& executableDirectory);
// Explicit native customization: read <executableDirectory>/launch and validate
// ALL mounts before entry. The default executable does not call this function.
// Throws on invalid configuration; does not execute Lua or change the root.
Config LoadManifest(const std::filesystem::path& executableDirectory);
}
