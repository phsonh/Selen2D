#pragma once
#include <SDL3/SDL_iostream.h>
#include <filesystem>
#include <string_view>
#include <optional>
#include <cstdint>
#include <array>
#include <exception>
namespace core::modules::File {
struct OpenMode;
namespace detail {
using NativeFileIdentity=std::array<std::uint64_t,3>; // Volume serial followed by the 128-bit file ID.
// Windows only: the caller retains a verified parent directory anchor until
// OpenNative returns. leaf is its actual single-component filesystem spelling.
// No identity means the caller verified that the target was missing.
// Windows requires a non-null target; other platforms retain the path backend.
struct NativeOpenTarget {
    void* parentDirectory{};
    std::filesystem::path leaf;
    std::optional<NativeFileIdentity> expectedIdentity;
    // A pending directory in another mapping prevents proving the location of
    // additional aliases. Recheck the live opened generation before any write.
    bool requireSingleLink{};
};
// Internal retry signal; never translate this directly into a public API error.
// The VFS must discard its view and revalidate before trying again.
class NativeViewChanged final : public std::exception {
public:
    const char* what() const noexcept override { return "native file identity changed"; }
};
SDL_IOStream* OpenNative(const std::filesystem::path&,const OpenMode&,std::string_view virtualPath,
    const NativeOpenTarget* target=nullptr);
std::optional<std::uint64_t> NativeModified(SDL_IOStream*,std::string_view virtualPath = {});
}
}
