# Vendored Zstandard

- Upstream: https://github.com/facebook/zstd/releases/tag/v1.5.7
- Fixed version: **1.5.7** (upstream `lib/` copied without source changes).
- Download: https://codeload.github.com/facebook/zstd/zip/refs/tags/v1.5.7
- Download SHA-256: `7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4`
- `LICENSE` contains the BSD 3-Clause option used by Solongo; upstream's alternative
  GPLv2 license is also retained as `COPYING`.
- Integration is in `../cmake/zstd/CMakeLists.txt`. Only common, compression and
  decompression C sources are built. Legacy codecs, assembly, dictionary training,
  multithread compression and upstream command-line programs are not enabled.
- No network access is needed for subsequent builds.
