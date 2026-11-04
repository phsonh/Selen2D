# Static libsodium integration

Source: unchanged official **libsodium 1.0.22** point release, downloaded from
https://download.libsodium.org/libsodium/releases/libsodium-1.0.22.tar.gz

Archive SHA256: `adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349`.
License: `3rd_party/libsodium/LICENSE` (ISC).

The official `.minisig` is verified with the maintainer key published at
https://doc.libsodium.org/installation :
`RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3`.
The reproducible local verification script is `tests/v2-package/verify_vendor.py`.
Its signing comment identifies the 2026-04-09 point-release tarball. No floating
stable snapshot, prebuilt DLL, or cryptographic implementation is substituted.

`DependencySodium` takes its source list directly from the upstream MSVC project
and follows upstream's static defines/include paths. The upstream version header
is copied into the build tree, leaving vendor source unchanged. This wrapper is
for the engine's Windows/MSVC build; it has no network or Python build step.
Consumers receive `SODIUM_STATIC` and link the system `advapi32` import library.
