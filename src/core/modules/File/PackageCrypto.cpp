#include "PackageCrypto.h"
#include <sodium.h>
#include <stdexcept>
namespace core::modules::File {
void InitializePackageCrypto() {
    static const bool initialized = [] {
        if (sodium_init() < 0)
            throw std::runtime_error("Resource package: cryptographic initialization failed");
        return true;
    }();
    (void)initialized;
}
PackageKey DerivePackageKey(std::string_view secret) {
    InitializePackageCrypto();
    PackageKey key{};
    crypto_hash_sha256(key.data(), reinterpret_cast<const unsigned char *>(secret.data()), secret.size());
    return key;
}
} // namespace core::modules::File
