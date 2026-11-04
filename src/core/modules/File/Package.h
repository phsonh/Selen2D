#pragma once
#include "Vfs.h"
#include "PackageCrypto.h"
#include <map>

namespace core::modules::File {
// Immutable validated binary index; independent, bounded, chunk-seekable read
// streams. The archive is not extracted and its entire PCM/image payload is
// never materialized. Each opened stream retains its archive metadata owner.
class Package final : public std::enable_shared_from_this<Package> {
  public:
    static std::shared_ptr<Package> Load(const std::filesystem::path &source, bool encrypted,
                                         std::optional<PackageKey> key = std::nullopt);
    ~Package();
    const std::map<std::string, NodeInfo, std::less<>> &Entries() const noexcept;
    NodeInfo Stat(std::string_view normalizedPath) const;
    std::optional<std::vector<std::string>> ListDirectory(std::string_view normalizedPath) const;
    SDL_IOStream *Open(std::string_view normalizedPath) const;
    // Compare the retained physical identity, including aliases after replacement.
    bool IsBackingFile(const std::filesystem::path &candidate) const;

  private:
    struct Impl;
    explicit Package(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace core::modules::File
