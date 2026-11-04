# Fixed Unicode path rules

CaseFolding-18.0.0.txt is the unmodified Unicode Character Database file:
https://www.unicode.org/Public/18.0.0/ucd/CaseFolding.txt

SHA256: A004797658A457BEC4DC11683E39F69249EA3B595B752DBEA6721C4C9F587B0D

LICENSE.txt is the Unicode license from https://www.unicode.org/license.txt.
Tools use full default case folding (C and F records, not Turkic T mappings),
followed by NFC from unicodedata2 18.0.0. They do not use Python's version-dependent
str.casefold. This matches utf8proc 2.12.0 in the native VFS.
