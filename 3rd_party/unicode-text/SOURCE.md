Pinned Unicode 17.0.0 vertical orientation data (UAX50).

Official source: https://www.unicode.org/Public/17.0.0/ucd/VerticalOrientation.txt
Joining data: https://www.unicode.org/Public/17.0.0/ucd/extracted/DerivedJoiningType.txt
License: https://www.unicode.org/license.txt (retained as LICENSE.txt).

SHA-256: VerticalOrientation.txt `dcef09c3fb24d356b042569c328ec341efc5b53447700d799f2fb4834c3cd3cd`; DerivedJoiningType.txt `f39ebe974825d6736aee15582250307aa532b2cfab3caf3f86bd23fddc9c5c4d`; LICENSE.txt `e7a93b009565cfce55919a381437ac4db883e9da2126fa28b91d12732bc53d96`.

The engine's committed `core/modules/Graphic/text/VerticalData.inc` and `JoiningData.inc` are lossless range/value transcriptions of these tables. They retain the Unicode attribution and are regenerated only by the local maintenance script `tests/v2-text/GenerateVertical.py`. Normal builds and runtime do not download or parse data files. U/Tu use upright fallback, R rotates clockwise, and Tr first checks the selected font's vertical substitution before using rotated fallback. Mongolian letters follow their Unicode R property: shape the joined horizontal OpenType run, then rotate the complete run clockwise for vertical layout. Required cursive connections use Unicode joining types, not HarfBuzz's unsafe-to-break optimization flag, which also marks ordinary kerning that is legal to reshape at a line break.
