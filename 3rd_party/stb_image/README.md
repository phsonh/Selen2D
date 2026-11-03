# stb_image dependency

Pinned original: stb_image v2.30, upstream commit
`f58f558c120e9b32c217290b80bad1a0729fbb2c`.
Imported from the read-only previous engine package cache, not SDL's modified private copy.
MIT / Unlicense text is retained in LICENSE; this project uses the MIT alternative.

Original SHA-256:

- stb_image.h: `1F8C1B6B408F26E3B20CBFBBD4758AFB3DC9B837FF1E17C258928F406148A87C`
- LICENSE: `11DD149DBC50748419A22FCEC275B5BCFB4C2D86D9FBA8802E5C4FFD40C94609`

One local hardening change is deliberately retained in stb_image.h:
`stbi__expand_png_palette` validates each decoded index against the actual PLTE
entry count before allocation/expansion. Upstream v2.30 ignores `len` and can read
uninitialized palette stack bytes for a malformed image whose index exceeds its
palette. Solongo rejects that input rather than returning nondeterministic RGBA.
Tests cover both unpacked and packed invalid indices. The read-only cache remains unchanged.

All engine-specific allocation limits, cancellation, PNG-only compile switches,
chunk/CRC validation and RAII live in `src/core/modules/Graphic/image/PngDecode.cpp`,
not in the vendor header. No SDL private API is used.
