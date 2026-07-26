# PCRE2 minimal subset

This directory vendors the minimal 8-bit PCRE2 static library subset needed by Feng.

Included:
- generated public header: include/pcre2.h
- generated build configuration: src/config.h
- generated chartables source: src/pcre2_chartables.c
- 8-bit core sources with Unicode/UTF enabled
- no 16/32-bit libraries
- no POSIX wrapper
- no tools, tests, or shared-library artifacts
- no SLJIT JIT dependency tree

Build:
- `make` builds the static library and stages it into `../../std/extlib/<host-platform>` by default.
- `make OUTPUT_DIR=<path>` overrides the staging directory.
- `make install` is an alias of the staging step.
- default staged library name: `libfeng_std_pcre2.a`
