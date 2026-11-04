#pragma once
#include <SDL3/SDL_iostream.h>
#include <filesystem>
#include <string_view>
#include <optional>
#include <cstdint>
namespace core::modules::File {
struct OpenMode;
namespace detail {
SDL_IOStream* OpenNative(const std::filesystem::path&,const OpenMode&,std::string_view virtualPath);
std::optional<std::uint64_t> NativeModified(SDL_IOStream*,std::string_view virtualPath = {});
}
}
