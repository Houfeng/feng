#!/usr/bin/env bash
# Verify archive completeness and actual function/data removal after ELF linking.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
source scripts/host_platform.sh
host=$(feng_detect_host_platform)
mkdir -p "$root/temp"
work=$(mktemp -d "$root/temp/release-sections.XXXXXX")
trap 'rm -rf "$work"' EXIT
compiler="$root/build/toolchain/llvm/bin/clang"
driver="$root/build/bin/test_llvm_c_eh_driver"
export SECTION_COMPILER="$compiler" SECTION_TRACE="$work/argv.txt"

# Record driver arguments while compiling real target objects.
cat > "$work/compiler" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" > "$SECTION_TRACE"
exec "$SECTION_COMPILER" "$@"
SH
chmod +x "$work/compiler"

cat > "$work/library.c" <<'C'
int section_used_data = 7;
int section_unused_data = 11;
/* Keep a mutable data reference so optimization cannot replace it by a constant. */
int section_used(void) { return section_used_data; }
/* A second consumer must still be able to select this public archive entry. */
int section_unused(void) { return section_unused_data; }
C
cat > "$work/main.c" <<'C'
extern int section_used(void);
/* Execute an actual archive call through the host's production link path. */
int main(void) { return section_used() != 7; }
C

# Fail with the symbol and artifact that violate the expected reachability.
check_symbol() {
    local map=$1 symbol=$2 expected=$3
    # Mach-O maps also list dead symbols after the live output; exclude that list.
    if awk -v symbol="$symbol" '
        /^# Dead Stripped Symbols:/ { exit }
        $NF == symbol || $NF == "_" symbol { found = 1 }
        END { exit !found }
    ' "$map"; then
        [[ $expected == present ]] || {
            echo "unexpected retained symbol $symbol in $map" >&2; exit 1;
        }
    else
        [[ $expected == absent ]] || {
            echo "missing live symbol $symbol in $map" >&2; exit 1;
        }
    fi
}

for target in macos-arm64 linux-x64-gnu linux-x64-musl linux-arm64-gnu linux-arm64-musl; do
    for release in 0 1; do
        archive="$work/$target-$release.a"
        FENG_CC_FLAGS= FENG_CC="$work/compiler" "$driver" \
            lib "$target" "$release" "$work/library.c" "$archive" -
        for flag in -ffunction-sections -fdata-sections; do
            if [[ $target == linux-* && $release == 1 ]]; then
                grep -Fx -- "$flag" "$SECTION_TRACE" >/dev/null || {
                    echo "missing $flag for $target release=$release" >&2; exit 1;
                }
            elif grep -Fx -- "$flag" "$SECTION_TRACE" >/dev/null; then
                echo "unexpected $flag for $target release=$release" >&2; exit 1
            fi
        done

        if [[ $target == linux-* ]]; then
            cpu=${target#linux-}
            cpu=${cpu%-*}
            if [[ $cpu == arm64 ]]; then cpu=aarch64; else cpu=x86_64; fi
            triple="$cpu-unknown-linux-${target##*-}"
            sysroot="$root/toolchain/sysroot/$target"
            # Select either API independently: archiving must not remove public
            # entries, while release ELF GC must remove the unselected pair.
            for selected in used unused; do
                other=unused
                if [[ $selected == unused ]]; then other=used; fi
                map="$work/$target-$release-$selected.map"
                "$compiler" --target="$triple" --sysroot="$sysroot" \
                    --gcc-toolchain="$sysroot" -fuse-ld=lld -nostdlib -static \
                    "-Wl,-e,section_$selected" "-Wl,-u,section_$selected" \
                    -Wl,--gc-sections "-Wl,-Map,$map" "$archive" \
                    -o "$work/$target-$release-$selected"
                check_symbol "$map" "section_$selected" present
                check_symbol "$map" "section_${selected}_data" present
                expected=present
                if [[ $release == 1 ]]; then expected=absent; fi
                check_symbol "$map" "section_$other" "$expected"
                check_symbol "$map" "section_${other}_data" "$expected"
            done
        fi

        if [[ $target == "$host" ]]; then
            map="$work/host-$release.map"
            map_flag="-Wl,-Map,$map"
            if [[ $host == macos-* ]]; then map_flag="-Wl,-map,$map"; fi
            FENG_CC_FLAGS="$map_flag" FENG_CC="$compiler" "$driver" \
                bin "$host" "$release" "$work/main.c" "$work/program" "$archive"
            "$work/program"
            check_symbol "$map" section_used_data present
            expected=present
            if [[ $release == 1 ]]; then expected=absent; fi
            check_symbol "$map" section_unused_data "$expected"
        fi
    done
done
echo 'PASS: release archives preserve both APIs; four Linux targets discard unused functions/data; host execution and debug/macOS flags verified'
