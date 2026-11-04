# Source import

Upstream: https://github.com/JuliaStrings/utf8proc/tree/v2.12.0
Version: 2.12.0; Unicode data: 18.0.0. Imported unmodified source, generated data,
header and upstream license/readme/news on 2026-11-04. Only the files needed by
the static library and attribution are vendored; upstream tests/build generators
are not required by this build.

SHA256:
- utf8proc.c: F38438EB986BA7EEDAD0F2E820A70FAE1AAF05197364BA0381A5E84775099EF8
- utf8proc.h: 08DA85E820EFAA3613500946C26B2DC427A07F76001ED7C16BC90F298B814C35
- utf8proc_data.c: B4911D4162D216AFA612317A9B981851E15D672F4D93B25F3A1EB986F8C0AEDB

The Python tools use unicodedata2 18.0.0 and the official Unicode 18 full case
folding table, matching the native path normalization/collision rules.
