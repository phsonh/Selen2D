# Orbit Garden asset sources

The example reads these files directly. No font download, Python or asset
generation is needed at runtime.

## Noto Sans Regular

`NotoSans-Regular.ttf` is the unmodified upstream font. `OFL.txt` is the original
upstream LICENSE, including its notice:

> Copyright 2018 The Noto Project Authors (github.com/googlei18n/noto-fonts)

The font remains under the SIL Open Font License 1.1. Keep `OFL.txt` with it;
the project's MIT license does not replace the font's license or attribution.

- Repository: https://github.com/notofonts/noto-fonts
- Fixed commit: `ffebf8c1ee449e544955a7e813c54f9b73848eac`
- Font: https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSans/NotoSans-Regular.ttf
- License: https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/LICENSE

Both files were checked byte-for-byte against those fixed official sources on
2026-11-04. Their exact SHA-256 values are:

| File | Bytes | SHA-256 |
|---|---:|---|
| NotoSans-Regular.ttf | 569208 | `B85C38ECEA8A7CFB39C24E395A4007474FA5A4FC864F6EE33309EB4948D232D5` |
| OFL.txt | 4377 | `0DAB92D0544F7B233403F14B84A663BDBFA746982EDA629E7F4F9FFE1B036FEB` |

## Original project assets

The following assets were generated for this example on 2026-11-04, without
external images, recordings or samples. They follow the project MIT license
and the existing copyright notice in the root LICENSE.

- `pixel.png`: a single opaque white RGBA8 pixel, used to tint UI rectangles.
- `sprite.png`: 32×32 white RGB with a soft circular alpha mask. For pixel-center
  radius `r=hypot(x+0.5-16,y+0.5-16)`, `q=clamp((r-11)/4,0,1)` and
  `alpha=round(255*(1-q*q*(3-2*q)))`.
- `bell.wav`: mono PCM16, 22050 Hz, 3308 frames (about 0.15 seconds). A weighted
  sum of 880/1320/1760 Hz sine waves (0.65/0.25/0.10), with 2 ms exponential
  attack, 40 ms exponential decay, final 10 ms sine-squared fade and gain 0.65.
  Endpoints are zero; there is no sampled recording.

| File | Bytes | SHA-256 |
|---|---:|---|
| pixel.png | 70 | `D5A51B6AED15684EC8C123E30FE5703155359D6543B6D7B47CB5766EF44939DE` |
| sprite.png | 410 | `FA0211EAC512CBA5BEB2E14935D7B5142D73FA68A23FAAA41EA6E29E29263B59` |
| bell.wav | 6660 | `EC5F93BD7AEE8FAE1119B16CF18BC504B63BCB8E15082AB5F32AEE4E299C993D` |
