#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace engine::launch {
struct Mount {
    std::string_view source, target;
    std::uint32_t parts;
    bool encrypted;
};
enum class Base { Deployment, UserData };
struct Directory {
    Base base;
    std::string_view path, target;
};
// Build-time data, never a Lua API or a runtime configuration parser.
struct Config {
    std::span<const Mount> mounts;
    std::span<const Directory> directories;
    std::string_view entry;
    // Missing differs from explicitly provided empty UTF-8 string.
    std::optional<std::span<const std::uint8_t>> secret;
};
}
