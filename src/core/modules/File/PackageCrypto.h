#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace core::modules::File {
using PackageKey = std::array<std::uint8_t, 32>;
void InitializePackageCrypto();
// Exactly SHA-256 of the supplied UTF-8 bytes, including embedded NUL bytes.
PackageKey DerivePackageKey(std::string_view secret);
} // namespace core::modules::File
