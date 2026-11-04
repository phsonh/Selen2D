Pinned official HarfBuzz 12.3.0, Unicode 17.0.0.

Source: https://github.com/harfbuzz/harfbuzz/tree/12.3.0
Archive: https://codeload.github.com/harfbuzz/harfbuzz/zip/refs/tags/12.3.0
Archive SHA-256: `7b529b17413993f6a9de5ccf87f7490642e63e76d53e9ef80e4e3808715901fd`

Retained upstream `src/` and non-hidden root files, including COPYING/AUTHORS and all source copyright notices. Tests, benchmarks, utilities and optional external dependencies are not imported. Upstream source is unmodified. The separate project wrapper builds upstream's unity translation unit as a static library, using its embedded OpenType shapers and Unicode tables. No operating-system font backend, dynamically loaded font library, downloaded build input or runtime network access is enabled.

License: upstream Old MIT; see COPYING and individual source notices. Unicode-derived data also retains its original notices; the project includes Unicode's license in `../unicode-text/LICENSE.txt`.

The wrapper uses upstream-supported `HB_BUFFER_MAX_LEN_*` / `HB_BUFFER_MAX_OPS_*` compile definitions to cap each shaping buffer at 262,144 glyphs/codepoints and 8,388,608 internal shaping operations. Limit exhaustion propagates as a real text backend error. Upstream source is not patched; these limits are part of the engine's documented bounded CPU text policy.
