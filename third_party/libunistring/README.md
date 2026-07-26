# libunistring minimal subset

This directory vendors the UTF-8 rune, grapheme, and case-mapping subset needed by Feng.

Public headers:
- include/unitypes.h
- include/feng_u8_rune.h
- include/feng_u8_grapheme.h
- include/feng_u8_case.h

Supported operations:
- UTF-8 rune count: u8_mbsnlen
- UTF-8 rune traversal: u8_next, u8_prev
- UTF-8 grapheme traversal: u8_grapheme_next, u8_grapheme_prev
- UTF-8 grapheme boundary map: u8_grapheme_breaks
- Unicode code point case mapping: uc_tolower, uc_toupper

Build:
- `make` builds the static library and stages it into `../../std/extlib/<host-platform>` by default.
- `make OUTPUT_DIR=<path>` overrides the staging directory.
- `make install` is an alias of the staging step.
- default staged library name: `libfeng_std_unistring.a`
