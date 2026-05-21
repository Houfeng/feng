# libunistring minimal subset

This directory vendors only the UTF-8 rune and grapheme subset needed by Feng.

Public headers:
- include/unitypes.h
- include/feng_u8_rune.h
- include/feng_u8_grapheme.h

Supported operations:
- UTF-8 rune count: u8_mbsnlen
- UTF-8 rune traversal: u8_next, u8_prev
- UTF-8 grapheme traversal: u8_grapheme_next, u8_grapheme_prev
- UTF-8 grapheme boundary map: u8_grapheme_breaks

Build:
- `make` builds the static library and stages it into `../../std/lib` by default.
- `make OUTPUT_DIR=<path>` overrides the staging directory.
- `make install` is an alias of the staging step.
- default staged library name: `libfeng_std_unistring.a`
