#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace core::modules::File {
// Strict nonempty relative UTF-8 path; rejects malformed syntax, then NFC.
std::optional<std::string> Normalize(std::string_view path);
// Full Unicode case folding followed by NFC, for collision keys only.
std::string FoldPath(std::string_view normalizedPath);
// Internal prefix relation: an empty parent denotes the configured virtual root.
// This does not make empty strings or '.' valid inputs to Normalize().
bool PathContains(std::string_view parent, std::string_view child) noexcept;
inline constexpr std::uint64_t MaxFilePosition = 9007199254740991ULL;
}
