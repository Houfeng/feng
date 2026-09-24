# A generic alternate entry with a configurable prefix, independent of sanitizers.
.text
.globl _main, _prefixed
.p2align 2
prefix_anchor:
  .space PREFIX_BYTES
.alt_entry _prefixed
_prefixed:
  .cfi_startproc
  .cfi_escape 0x2e, 0
  ret
  .cfi_endproc
_main:
  ret
.subsections_via_symbols
