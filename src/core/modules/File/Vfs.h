#pragma once
#include "File.h"
#include <SDL3/SDL_iostream.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace core::modules::File {
// Paths are UTF-8 virtual paths. Empty / "." denotes the virtual root;
// normalization never escapes that root. FileSystem also protects mount borders.
std::optional<std::string> Normalize(std::string_view path);

enum class NodeType { Missing, File, Directory };
struct NodeInfo {
    NodeType type{NodeType::Missing};
    std::uint64_t size{}, modified{};
};
struct MountSpec { std::filesystem::path source; std::string target; };
struct OpenMode {
    char base{};
    bool readable{}, writable{}, append{}, exclusive{};
    std::string sdlMode;
};
// Malformed modes are programmer errors; normal filesystem failures are not.
OpenMode ParseMode(std::string_view mode);

// One immutable mount snapshot per host. The constructor validates every mount
// before publication. Concurrent readers own separate streams; no shared seek
// cursor. The install directory is writable; mounted regions are read-only.
// Directory naming belongs to Lua. No process-current-directory dependency.
class FileSystem final {
public:
    FileSystem(std::filesystem::path installRoot, std::vector<MountSpec> mounts);
    ~FileSystem();
    FileSystem(const FileSystem&) = delete;
    FileSystem& operator=(const FileSystem&) = delete;
    std::optional<std::string> NormalizePath(std::string_view path) const;
    NodeInfo Stat(std::string_view path) const;
    std::optional<std::vector<std::string>> ListDirectory(std::string_view path) const;
    // Caller owns the SDL stream and must SDL_CloseIO it; null means I/O failure.
    SDL_IOStream* Open(std::string_view path, const OpenMode& mode) const;
    SDL_IOStream* Open(std::string_view path, std::string_view mode) const;
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
