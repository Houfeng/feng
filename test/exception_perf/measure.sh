#!/usr/bin/env bash
# Alternate all requested builds; never make noisy wall-clock deltas a CI assertion.
set -euo pipefail
if [[ $# -lt 2 ]]; then echo "usage: $0 RESULTS_TSV BUILD_DIR..." >&2; exit 2; fi
results=$1
shift
printf 'round\tbuild\tlevel\tlayout\tlanguage\tmode\tcount\tnanoseconds\tchecksum\n' > "$results"
rounds=${EH_ROUNDS:-7}
modes=${EH_MODES:-0 1 2 3 4 5 6 7 8}
for ((round=0; round<=rounds; ++round)); do
    # Reverse paired order each round to avoid always favoring a warm/boosted build.
    dirs=("$@")
    for level in 0 2 3; do
        for layout in single split; do
            for mode in $modes; do
                count=20000000
                if ((mode == 6)); then count=200000; fi
                if ((mode == 7)); then count=20000; fi
                for ((index=0; index<${#dirs[@]}; ++index)); do
                    selected=$index
                    if ((round % 2)); then selected=$((${#dirs[@]} - index - 1)); fi
                    directory=${dirs[$selected]}
                    for language in feng cpp; do
                        binary="$directory/O$level-$layout/cpp"
                        if [[ "$language" == feng ]]; then
                            binary="$directory/O$level-$layout/consumer/build/bin/eh_bench"
                        fi
                        line=$(EH_COUNT="$count" EH_MODE="$mode" "$binary")
                        if ((round > 0)); then
                            printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$round" "$directory" \
                                "$level" "$layout" "$language" "$line" >> "$results"
                        fi
                    done
                done
            done
        done
    done
    echo "exception performance sample $round/$rounds complete" >&2
done
