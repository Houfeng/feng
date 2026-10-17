#!/usr/bin/env bash
# Read both array elements through native LLDB, without Feng or .fd mappings.
set -euo pipefail
[[ $# -eq 2 ]] || { echo 'usage: native-lldb.sh LLDB VALIDATION_DIRECTORY' >&2; exit 1; }
lldb=$1
output=$2
for mode in off thin; do
    "$lldb" --batch "$output/$mode/fixture" \
        -o 'settings set symbols.enable-external-lookup false' \
        -o 'breakpoint set --name inspect_records' -o run \
        -o 'expression -- ((struct LocalRecord *)local)[0].tag == 1 && ((struct LocalRecord *)local)[0].payload.number == 7 && ((struct LocalRecord *)local)[0].weight == 1.5' \
        -o 'expression -- ((struct LocalRecord *)local)[1].tag == 2 && ((struct LocalRecord *)local)[1].payload.number == 19 && ((struct LocalRecord *)local)[1].weight == 4.25' \
        -o 'expression -- (int)((struct RemoteRecord *)remote)[0].tag == 1 && ((struct RemoteRecord *)remote)[0].value.small == 31' \
        -o 'expression -- (int)((struct RemoteRecord *)remote)[1].tag == 2 && ((struct RemoteRecord *)remote)[1].value.large == 9000000042L' \
        -o quit > "$output/$mode/lldb.log" 2>&1
    [[ $(grep -Ec '\(bool\) \$[0-9]+ = true' "$output/$mode/lldb.log") == 4 ]] || {
        cat "$output/$mode/lldb.log" >&2
        echo "native LLDB array expansion failed: $mode" >&2
        exit 1
    }
done
echo 'native LLDB: all local and archive array elements passed off/thin'
