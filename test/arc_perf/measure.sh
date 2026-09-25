#!/usr/bin/env bash
# Alternate build order and validate every sample before recording it.
set -euo pipefail
if [[ $# -lt 3 ]]; then echo "usage: $0 OUTPUT_TSV BUILD_DIR BUILD_DIR..." >&2; exit 2; fi
output=$1; shift
builds=("$@")
mkdir -p -- "$(dirname -- "$output")"
printf 'round\tbuild\toptimization\tlayout\tmode\tcount\tns\tchecksum\tpeak\tdrops\tdefaults\n' > "$output"
for level in 0 2 3; do
    for layout in single split; do
        for mode in ${ARC_MODES:-0 1 2 3 4 5 6 7 8 9 10}; do
            count=${ARC_COUNT:-1000000}; if [[ $mode == 9 ]]; then count=${ARC_THROW_COUNT:-100000}; fi
            for build in "${builds[@]}"; do
                ARC_COUNT=1001 ARC_MODE="$mode" "$build/O$level-$layout/consumer/bin/arc_bench" >/dev/null
            done
            for ((round=0; round<${ARC_ROUNDS:-7}; ++round)); do
                for ((index=0; index<${#builds[@]}; ++index)); do
                    chosen=$index
                    if ((round % 2)); then chosen=$((${#builds[@]}-1-index)); fi
                    build=${builds[$chosen]}
                    result=$(ARC_COUNT="$count" ARC_MODE="$mode" "$build/O$level-$layout/consumer/bin/arc_bench")
                    printf '%s\t%s\tO%s\t%s\t%s\n' "$round" "$(basename -- "$build")" "$level" "$layout" "$result" >> "$output"
                done
            done
        done
    done
done
