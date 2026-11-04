#pragma once
#include "File.h"
#include "Path.h"
#include "IoError.h"
#include <SDL3/SDL_iostream.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <array>

namespace core::modules::File {
class ReadSource;
enum class NodeType { Missing, File, Directory };
struct NodeInfo {
    NodeType type{NodeType::Missing};
    std::uint64_t size{}, modified{};
};
struct MountSpec {
    std::filesystem::path source; // Strict path relative to executable directory.
    std::string target;
    std::uint32_t parts{};
    bool encrypted{};
};
struct DirectorySpec {
    std::filesystem::path baseRoot; // Absolute system/deployment root from Startup.
    std::string path;              // Nonempty strict relative project directory.
    std::string target;            // Nonempty virtual directory alias.
};
struct OpenMode {
    char base{};
    bool readable{}, writable{}, append{}, exclusive{};
    std::string sdlMode;
};
// Malformed modes are programmer errors; normal filesystem failures are not.
OpenMode ParseMode(std::string_view mode);

// Fixed mount/authorization snapshot, with no implicit installation-root access.
// Missing declared directories retain creation routes but are not auto-created.
class FileSystem final {
public:
    FileSystem(std::filesystem::path installRoot, std::vector<MountSpec> mounts,
        std::vector<DirectorySpec> directories,
        std::optional<std::array<std::uint8_t,32>> key);
    ~FileSystem();
    FileSystem(const FileSystem&) = delete;
    FileSystem& operator=(const FileSystem&) = delete;
    // Optional retained for existing consumers; invalid syntax/authorization
    // throws. A successful result always has a value.
    std::optional<std::string> NormalizePath(std::string_view path) const;
    // Existing file code in an encrypted configuration must be authenticated
    // package content. An authorized missing candidate remains missing.
    void CheckCodeSource(std::string_view path) const;
    NodeInfo Stat(std::string_view path) const;
    std::optional<std::vector<std::string>> ListDirectory(std::string_view path) const;
    // Caller owns the SDL stream. Ordinary failures throw IoError; parameter,
    // authorization and structural errors use distinct exception classes.
    SDL_IOStream* Open(std::string_view path, const OpenMode& mode) const;
    SDL_IOStream* Open(std::string_view path, std::string_view mode) const;
    // Retain an authorized open backend for streaming resources. New cursors
    // keep this generation even after the original pathname is replaced.
    std::shared_ptr<const ReadSource> OpenReadSource(std::string_view path) const;
    bool CreateDirectory(std::string_view path) const;
    bool Remove(std::string_view path) const;
    bool Rename(std::string_view directory, std::string_view oldName, std::string_view newName) const;
    bool Copy(std::string_view source, std::string_view destination) const;
    bool Move(std::string_view source, std::string_view destination) const;
    bool Replace(std::string_view source, std::string_view destination) const;
    // Resource workers retain the existing admission / cancellation contract.
    Snapshot ReadSnapshot(std::string_view path, const ReadOptions& options) const;
    std::optional<std::string> Read(std::string_view path) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
