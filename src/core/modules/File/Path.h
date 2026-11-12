#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <span>

namespace core::modules::File {
// Strict nonempty relative UTF-8 path; rejects malformed syntax, then NFC.
std::optional<std::string> Normalize(std::string_view path);
// Full Unicode case folding followed by NFC, for collision keys only.
std::string FoldPath(std::string_view normalizedPath);
// Internal prefix relation: an empty parent denotes the configured virtual root.
// This does not make empty strings or '.' valid inputs to Normalize().
bool PathContains(std::string_view parent, std::string_view child) noexcept;
inline constexpr std::uint64_t MaxFilePosition = 9007199254740991ULL;
namespace Path {
// Pure string operations: strict path validation and NFC, no VFS authorization.
std::string Join(std::span<const std::string_view> parts);
std::string GetFileName(std::string_view path);
std::optional<std::string> GetParent(std::string_view path);
std::string GetExtension(std::string_view path);
// Internal name construction shared by ChangeExtension; not a Lua API.
std::string ReplaceExtension(std::string_view path,std::string_view extension);
}
}
