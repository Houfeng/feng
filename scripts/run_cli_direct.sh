#!/usr/bin/env bash
# Direct-mode CLI surface checks for Phase 2 P4 + P5.
#
# Verifies that `feng <file> --target=bin --out=<dir>` drives the full
# pipeline (frontend -> codegen -> host cc) and produces a runnable
# executable, that error paths return non-zero with actionable
# diagnostics, and that `--keep-ir` preserves the intermediate C file.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FENG="$ROOT/build/bin/feng"
RT_LIB="$ROOT/build/lib/libfeng_runtime.a"
FIXTURE="$ROOT/test/smoke/phase1a/hello.ff"
EXPECTED="$ROOT/test/smoke/phase1a/hello.expected"
WORK="$(mktemp -d -t feng_cli_XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

if [[ ! -x "$FENG" ]]; then
    echo "missing $FENG (run 'make cli' first)" >&2
    exit 2
fi
if [[ ! -f "$RT_LIB" ]]; then
    echo "missing $RT_LIB (run 'make runtime' first)" >&2
    exit 2
fi
if [[ ! -f "$FIXTURE" ]]; then
    echo "missing fixture $FIXTURE" >&2
    exit 2
fi

failures=0

expect_ok() {
    local label="$1"; shift
    if ! "$@" >"$WORK/$label.out" 2>"$WORK/$label.err"; then
        local rc=$?
        echo "FAIL[$label] expected success, exit=$rc"
        sed 's/^/  /' "$WORK/$label.err"
        failures=$((failures + 1))
        return 1
    fi
    return 0
}

expect_fail() {
    local label="$1"; shift
    if "$@" >"$WORK/$label.out" 2>"$WORK/$label.err"; then
        echo "FAIL[$label] expected failure, but command succeeded"
        failures=$((failures + 1))
        return 1
    fi
    return 0
}

# 0. help output should use the executable basename, not the invoked path
if expect_ok "help" "$FENG" --help; then
    if ! grep -q "^  feng " "$WORK/help.err"; then
        echo "FAIL[help] usage should show executable name 'feng'"
        failures=$((failures + 1))
    fi
    if grep -q "$FENG" "$WORK/help.err"; then
        echo "FAIL[help] usage should not echo the invoked executable path"
        failures=$((failures + 1))
    fi
fi

# 1. happy path: full pipeline produces a runnable binary
out1="$WORK/case_full"
if expect_ok "full_pipeline" "$FENG" "$FIXTURE" --target=bin --out="$out1"; then
    bin="$out1/bin/hello"
    if [[ ! -x "$bin" ]]; then
        echo "FAIL[full_pipeline] missing executable $bin"
        failures=$((failures + 1))
    else
        actual="$("$bin")"
        expected_text="$(cat "$EXPECTED")"
        if [[ "$actual" != "$expected_text" ]]; then
            echo "FAIL[full_pipeline] stdout mismatch"
            echo "  expected: $expected_text"
            echo "  actual:   $actual"
            failures=$((failures + 1))
        fi
    fi
    if [[ -f "$out1/ir/c/feng.c" ]]; then
        echo "FAIL[full_pipeline] IR file should be cleaned without --keep-ir"
        failures=$((failures + 1))
    fi
    if [[ -d "$out1/ir" ]]; then
        echo "FAIL[full_pipeline] empty IR directory should be cleaned without --keep-ir"
        failures=$((failures + 1))
    fi
fi

# 2. --keep-ir preserves the intermediate C file
out2="$WORK/case_keep"
if expect_ok "keep_ir" "$FENG" "$FIXTURE" --out="$out2" --keep-ir; then
    if [[ ! -f "$out2/ir/c/feng.c" ]]; then
        echo "FAIL[keep_ir] missing $out2/ir/c/feng.c"
        failures=$((failures + 1))
    fi
fi

# 3. --target=lib should produce a static archive under <out>/lib
out3="$WORK/case_lib"
if expect_ok "lib_static" "$FENG" "$FIXTURE" --target=lib --out="$out3" --name=hello_lib; then
    lib="$out3/lib/libhello_lib.a"
    if [[ ! -f "$lib" ]]; then
        echo "FAIL[lib_static] missing archive $lib"
        failures=$((failures + 1))
    elif ! ar -t "$lib" | grep -q '^feng.o$'; then
        echo "FAIL[lib_static] archive does not contain feng.o"
        failures=$((failures + 1))
    fi
    if [[ -f "$out3/ir/c/feng.c" ]]; then
        echo "FAIL[lib_static] IR file should be cleaned without --keep-ir"
        failures=$((failures + 1))
    fi
fi

# 4. missing --out
expect_fail "no_out" "$FENG" "$FIXTURE" --target=bin || true
if ! grep -q -- "--out=<dir> is required" "$WORK/no_out.err"; then
    echo "FAIL[no_out] missing --out diagnostic"
    failures=$((failures + 1))
fi

# 5. unknown option
expect_fail "unknown_opt" "$FENG" "$FIXTURE" --out="$WORK/case_unk" --bogus || true
if ! grep -q "unknown option: --bogus" "$WORK/unknown_opt.err"; then
    echo "FAIL[unknown_opt] missing unknown-option diagnostic"
    failures=$((failures + 1))
fi

# 6. legacy top-level command still rejected with migration hint
expect_fail "legacy_lex" "$FENG" lex "$FIXTURE" || true
if ! grep -q "use .*tool lex" "$WORK/legacy_lex.err"; then
    echo "FAIL[legacy_lex] missing migration hint"
    failures=$((failures + 1))
fi

# 7. runtime override pointing at a missing path is reported clearly
out7="$WORK/case_rtmiss"
if FENG_RUNTIME_LIB="$WORK/no_such_runtime.a" "$FENG" "$FIXTURE" --out="$out7" \
       >"$WORK/rt_missing.out" 2>"$WORK/rt_missing.err"; then
    echo "FAIL[rt_missing] expected failure with FENG_RUNTIME_LIB pointing nowhere"
    failures=$((failures + 1))
elif ! grep -q "FENG_RUNTIME_LIB points to" "$WORK/rt_missing.err"; then
    echo "FAIL[rt_missing] missing FENG_RUNTIME_LIB diagnostic"
    failures=$((failures + 1))
fi

# 8. failing host cc surfaces a clear error and preserves the IR file
out8="$WORK/case_ccfail"
fake_cc_dir="$WORK/fake_cc"
mkdir -p "$fake_cc_dir"
cat >"$fake_cc_dir/cc" <<'INNER'
#!/usr/bin/env bash
echo "fake cc: simulated failure" >&2
exit 7
INNER
chmod +x "$fake_cc_dir/cc"
if CC="$fake_cc_dir/cc" "$FENG" "$FIXTURE" --out="$out8" \
       >"$WORK/cc_fail.out" 2>"$WORK/cc_fail.err"; then
    echo "FAIL[cc_fail] expected failure when CC stub returns non-zero"
    failures=$((failures + 1))
elif ! grep -q "host C compiler failed" "$WORK/cc_fail.err"; then
    echo "FAIL[cc_fail] missing host cc failure diagnostic"
    failures=$((failures + 1))
elif [[ ! -f "$out8/ir/c/feng.c" ]]; then
    echo "FAIL[cc_fail] IR file should be preserved on cc failure"
    failures=$((failures + 1))
fi

# 9. multi-file compile: two .ff files contributing to the same module,
#    produced artifact name controlled via --name.
MULTI_DIR="$ROOT/test/smoke/phase1a/multi_hello"
MULTI_EXPECTED="$ROOT/test/smoke/phase1a/multi_hello.expected"
out9="$WORK/case_multi"
if [[ -d "$MULTI_DIR" ]]; then
    multi_files=()
    while IFS= read -r f; do
        multi_files+=("$f")
    done < <(find "$MULTI_DIR" -maxdepth 1 -name '*.ff' | sort)
        if expect_ok "multi_file" "$FENG" "${multi_files[@]}" --target=bin \
            --out="$out9" --name=multi_hello; then
        bin="$out9/bin/multi_hello"
        if [[ ! -x "$bin" ]]; then
            echo "FAIL[multi_file] missing executable $bin"
            failures=$((failures + 1))
        else
            actual="$("$bin")"
            expected_text="$(cat "$MULTI_EXPECTED")"
            if [[ "$actual" != "$expected_text" ]]; then
                echo "FAIL[multi_file] stdout mismatch"
                echo "  expected: $expected_text"
                echo "  actual:   $actual"
                failures=$((failures + 1))
            fi
        fi
    fi
else
    echo "FAIL[multi_file] missing fixture $MULTI_DIR"
    failures=$((failures + 1))
fi

# 10. --name with an empty value is rejected
expect_fail "name_empty" "$FENG" "$FIXTURE" --out="$WORK/case_name" \
    --name= || true
if ! grep -q -- "--name requires a non-empty value" "$WORK/name_empty.err"; then
    echo "FAIL[name_empty] missing --name diagnostic"
    failures=$((failures + 1))
fi

# 11. top-level compile should redirect users to tool compile
expect_fail "legacy_compile_redirect" "$FENG" compile --emit-c="$WORK/legacy.c" "$FIXTURE" || true
if ! grep -q "use .*tool compile" "$WORK/legacy_compile_redirect.err"; then
    echo "FAIL[legacy_compile_redirect] missing migration hint"
    failures=$((failures + 1))
fi

# 12. tool compile remains available for compiler-development debug flows
tool_c="$WORK/tool_compile.c"
if expect_ok "tool_compile" "$FENG" tool compile --emit-c="$tool_c" "$FIXTURE"; then
    if [[ ! -s "$tool_c" ]]; then
        echo "FAIL[tool_compile] missing generated C output"
        failures=$((failures + 1))
    elif ! grep -q "int main" "$tool_c"; then
        echo "FAIL[tool_compile] generated C did not contain main wrapper"
        failures=$((failures + 1))
    fi
fi

if [[ $failures -gt 0 ]]; then
    echo "cli (direct mode): $failures failure(s)"
    exit 1
fi
echo "cli (direct mode): all checks passed"
