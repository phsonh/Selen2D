# Licenses are retained; isolated hardening patches are documented per dependency.
# Build outputs belong in build/, never here.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_FORCE_STATIC_VCRT ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL EXCLUDE_FROM_ALL)

add_subdirectory(cmake/luajit)
add_subdirectory(cmake/audio)
add_subdirectory(cmake/zstd)

# Pinned upstream FreeType VER-2-14-1 (526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b).
# Fonts are prepared from VFS memory; upstream source and licenses are intact.
# This stage needs SFNT TrueType/OpenType parsing, not webfonts, shaping or
# bitmap/color-glyph decoding. Keep optional dependency discovery deterministic.
block(SCOPE_FOR VARIABLES)
    set(BUILD_SHARED_LIBS OFF)
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(FT_ENABLE_ERROR_STRINGS ON CACHE BOOL "" FORCE)
    add_subdirectory(freetype EXCLUDE_FROM_ALL)
endblock()
add_library(DependencyFreeType ALIAS freetype)

add_library(DependencyJsonHeaders INTERFACE)
target_include_directories(DependencyJsonHeaders INTERFACE nlohmann_json/include)
