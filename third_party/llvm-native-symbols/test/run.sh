#!/usr/bin/env bash
# Consume a Release plugin; compare native behavior with an unmodified baseline.
set -euo pipefail
if [[ $# -lt 3 ]]; then echo 'usage: run.sh CLANG PLUGIN OUTPUT [--sanitizer=LIST] [--link-option=OPTION]' >&2; exit 2; fi
compiler=$1
plugin=$2
output=$3
shift 3
sanitizer=
strict_fp=false
link_options=()
for option in "$@"; do
    case "$option" in
        --sanitizer) sanitizer=undefined;;
        --sanitizer=*) sanitizer=${option#*=};;
        --strict-fp) strict_fp=true;;
        --link-option=*) link_options+=("${option#*=}");;
        *) echo "unknown option: $option" >&2; exit 2;;
    esac
done
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$output"
"$compiler" --version > "$output/compiler.txt"
grep -Eq 'clang version 22\.1\.8([[:space:]]|$)' "$output/compiler.txt"
flags=(-std=c11 -g -Wall -Wextra -Werror)
if [[ $(uname -s) == Darwin ]]; then flags+=(-isysroot "$(xcrun --show-sdk-path)"); fi
if [[ -n "$sanitizer" ]]; then flags+=("-fsanitize=$sanitizer" -fno-sanitize-recover=all); fi
if "$strict_fp"; then flags+=(-ffp-model=strict); fi
for level in 0 2 3; do
    for mode in baseline plugin; do
        plugin_options=()
        if [[ $mode == plugin ]]; then plugin_options+=("-fpass-plugin=$plugin"); fi
        "$compiler" "${flags[@]}" -O"$level" ${plugin_options[@]+"${plugin_options[@]}"} \
            "$test_dir/producer.c" "$test_dir/behavior.c" -lm \
            ${link_options[@]+"${link_options[@]}"} -o "$output/$mode-O$level"
        "$output/$mode-O$level" > "$output/$mode-O$level.txt"
    done
    cmp "$output/baseline-O$level.txt" "$output/plugin-O$level.txt"
done
echo 'PASS: native aliases, callable identity, side effects, floating boundaries and errno'
