# Windows application icon

`selen2d-logo.ico` is a checked-in build input derived from the original
`../image/selen2d-logo.png` (1024 × 1024 RGBA). It contains 16, 24, 32, 48,
64, 128 and 256 pixel images with 32-bit RGBA transparency. The source artwork
is preserved without cropping or recolouring. The current artwork is the
user-provided `selen2d-logo-v2.png`, copied unchanged into the source PNG path.

One-time generation used Python with Pillow 12.2.0, from the repository root:

```python
from PIL import Image

with Image.open("src/res/image/selen2d-logo.png") as source:
    assert source.size == (1024, 1024)
    source.convert("RGBA").save(
        "src/res/windows/selen2d-logo.ico",
        format="ICO",
        sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)],
    )
```

Pillow resamples each size using Lanczos and stores PNG-compressed RGBA entries
inside the ICO. Ordinary builds consume the ICO directly; no image conversion
tool or network access is needed.

`app-icon.rc.in` belongs only to `engine_app`. The shared version resource does not include the icon; command-line tools
do not inherit this application-only resource. SDL's existing Windows window-class
initialization discovers the executable's icon resource. The development
runtime package contains neither the source PNG nor the ICO as separate files.
