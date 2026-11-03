# Vendored Zstandard 1.5.7, BSD-licensed option (see zstd/LICENSE and COPYING).
# Package blocks are independent frames; no legacy formats, assembler, workers,
# programs, dictionary trainer or shared runtime dependency are required.
set(dependency_zstd_root "${CMAKE_CURRENT_LIST_DIR}/../../zstd")
file(GLOB dependency_zstd_common CONFIGURE_DEPENDS "${dependency_zstd_root}/common/*.c")
file(GLOB dependency_zstd_compress CONFIGURE_DEPENDS "${dependency_zstd_root}/compress/*.c")
file(GLOB dependency_zstd_decompress CONFIGURE_DEPENDS "${dependency_zstd_root}/decompress/*.c")
add_library(DependencyZstd STATIC ${dependency_zstd_common} ${dependency_zstd_compress} ${dependency_zstd_decompress})
target_include_directories(DependencyZstd PUBLIC "${dependency_zstd_root}")
target_compile_definitions(DependencyZstd PRIVATE ZSTD_LEGACY_SUPPORT=0 ZSTD_DISABLE_ASM=1)
if(MSVC)
    target_compile_options(DependencyZstd PRIVATE /utf-8)
    target_compile_definitions(DependencyZstd PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
