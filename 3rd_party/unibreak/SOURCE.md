Pinned official libunibreak 8.0, Unicode 17.0.0 (UAX14 revision 55 and UAX29 revision 47).

Source: https://github.com/adah1972/libunibreak/tree/libunibreak_8_0
Archive: https://codeload.github.com/adah1972/libunibreak/zip/refs/tags/libunibreak_8_0
Archive SHA-256: `a50cb9d2f557386470ee5cb92c75507afbb52778e237cfae129397e2d491e9f4`

Retained unmodified upstream `src/` and non-hidden root files. The separate project wrapper compiles the line/grapheme algorithms and their checked-in generated data as a static library. Upstream regeneration recipes are retained as provenance, but no build target downloads or regenerates Unicode data. The word-break API is not linked because the engine exposes neither a word iterator nor text editing.

License: zlib/libpng, see LICENCE and source notices. Unicode-derived data also retains its original notices; Unicode license is additionally available in `../unicode-text/LICENSE.txt`.
