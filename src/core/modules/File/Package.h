#pragma once
#include "Vfs.h"

namespace core::modules::File {
// Immutable validated binary index; independent, bounded, chunk-seekable read
// streams. The archive is not extracted and its entire PCM/image payload is
// never materialized. Each opened stream retains its archive metadata owner.
class Package final : public std::enable_shared_from_this<Package> {
public:
    static std::shared_ptr<Package> Load(const std::filesystem::path& source);
    ~Package();
    NodeInfo Stat(std::string_view normalizedPath) const;
    std::optional<std::vector<std::string>> ListDirectory(std::string_view normalizedPath) const;
    SDL_IOStream* Open(std::string_view normalizedPath) const;
private:
    struct Impl;
    explicit Package(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
// Native tooling only, not a Lua File API. Source is a real directory; output
// must not exist. Validated paths, bounded chunks, deterministic index ordering.
void BuildPackage(const std::filesystem::path& directory,
                  const std::filesystem::path& output,
                  std::uint32_t chunkBytes = 256 * 1024);
}
