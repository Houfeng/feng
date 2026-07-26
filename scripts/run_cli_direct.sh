#!/usr/bin/env bash
# Direct-mode CLI surface checks for Phase 2 P4 + P5.
#
# Verifies that `feng <file> --target=bin [--platform=<platform>] [--out=<dir>]`
# drives the full
# pipeline (frontend -> codegen -> host cc) and produces a runnable
# executable, that error paths return non-zero with actionable
# diagnostics, and that `--keep-ir` preserves the intermediate C file.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/host_platform.sh"
FENG="$ROOT/build/bin/feng"
HOST_PLATFORM="$(feng_detect_host_platform)"
RT_LIB="$ROOT/build/lib/$HOST_PLATFORM/libfeng_runtime.a"
FIXTURE="$ROOT/test/smoke/phase1a/hello.ff"
EXPECTED="$ROOT/test/smoke/phase1a/hello.expected"
WORK="$ROOT/build/gen/cli_direct"
rm -rf "$WORK"
mkdir -p "$WORK"
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
    if ! grep -q "^  feng " "$WORK/help.out"; then
        echo "FAIL[help] usage should show executable name 'feng'"
        failures=$((failures + 1))
    fi
    if grep -q "$FENG" "$WORK/help.out"; then
        echo "FAIL[help] usage should not echo the invoked executable path"
        failures=$((failures + 1))
    fi
    if [[ -s "$WORK/help.err" ]]; then
        echo "FAIL[help] help command should not write stderr"
        failures=$((failures + 1))
    fi
    if ! grep -q '^  feng <files>\.\.\. \[options\]$' "$WORK/help.out"; then
        echo "FAIL[help] missing direct compile usage line"
        failures=$((failures + 1))
    fi
    if ! grep -q '^  feng <command>  \[options\]$' "$WORK/help.out"; then
        echo "FAIL[help] missing command usage line"
        failures=$((failures + 1))
    fi
    if ! grep -q '^Project:$' "$WORK/help.out"; then
        echo "FAIL[help] missing Project section"
        failures=$((failures + 1))
    fi
    if ! grep -Eq '^Compile:$' "$WORK/help.out"; then
        echo "FAIL[help] missing Compile section"
        failures=$((failures + 1))
    fi
    if ! grep -Eq '^  feng <files>\.\.\. \[--target=bin\|lib\][[:space:]]*$' "$WORK/help.out"; then
        echo "FAIL[help] missing wrapped compile header line"
        failures=$((failures + 1))
    fi
    if ! grep -Eq '^[[:space:]]+\[--platform=<platform>\][[:space:]]*$' "$WORK/help.out"; then
        echo "FAIL[help] missing optional wrapped --platform line"
        failures=$((failures + 1))
    fi
    if ! grep -Eq '^[[:space:]]+\[--out=<dir>\][[:space:]]*$' "$WORK/help.out"; then
        echo "FAIL[help] missing optional wrapped --out line"
        failures=$((failures + 1))
    fi
    if ! grep -Eq '^[[:space:]]+\[--name=<artifact>\][[:space:]]*$' "$WORK/help.out"; then
        echo "FAIL[help] missing wrapped --name line"
        failures=$((failures + 1))
    fi
    if ! grep -Eq '^[[:space:]]+\[--release\][[:space:]]*$' "$WORK/help.out"; then
        echo "FAIL[help] missing wrapped --release line"
        failures=$((failures + 1))
    fi
    if ! grep -Eq '^[[:space:]]+\[--keep-ir\]$' "$WORK/help.out"; then
        echo "FAIL[help] missing wrapped --keep-ir line"
        failures=$((failures + 1))
    fi
    if ! grep -q '^Global:$' "$WORK/help.out"; then
        echo "FAIL[help] missing Global section"
        failures=$((failures + 1))
    fi
    if ! grep -q '^  -h, --help      Display this message\.$' "$WORK/help.out"; then
        echo "FAIL[help] missing --help description line"
        failures=$((failures + 1))
    fi
    if ! grep -q '^  -v, --version   Display version information\.$' "$WORK/help.out"; then
        echo "FAIL[help] missing --version description line"
        failures=$((failures + 1))
    fi
    if ! grep -Eq '^Protocol:[[:space:]]*$' "$WORK/help.out"; then
        echo "FAIL[help] missing Protocol section"
        failures=$((failures + 1))
    fi
    if ! grep -q '^  feng lsp        \[--stdio\]$' "$WORK/help.out"; then
        echo "FAIL[help] missing lsp usage line"
        failures=$((failures + 1))
    fi

    project_line=$(grep -n '^Project:$' "$WORK/help.out" | head -n1 | cut -d: -f1)
    compile_line=$(grep -n '^Compile:$' "$WORK/help.out" | head -n1 | cut -d: -f1)
    global_line=$(grep -n '^Global:$' "$WORK/help.out" | head -n1 | cut -d: -f1)
    editor_line=$(grep -n '^Protocol:[[:space:]]*$' "$WORK/help.out" | head -n1 | cut -d: -f1)
    if [[ -z "$project_line" || -z "$compile_line" || -z "$global_line" || -z "$editor_line" ]] \
        || (( project_line >= compile_line )) \
        || (( compile_line >= editor_line )) \
        || (( editor_line >= global_line )); then
        echo "FAIL[help] usage sections are out of order"
        failures=$((failures + 1))
    fi
fi

# 0.0 version output should be available as a documented global option
if expect_ok "version" "$FENG" --version; then
    if ! grep -Eq '^feng .+$' "$WORK/version.out"; then
        echo "FAIL[version] unexpected version output"
        failures=$((failures + 1))
    fi
    if [[ -s "$WORK/version.err" ]]; then
        echo "FAIL[version] version command should not write stderr"
        failures=$((failures + 1))
    fi
fi

# 0.1 lsp help exposes stdio contract
if expect_ok "lsp_help" "$FENG" lsp --help; then
    if [[ -s "$WORK/lsp_help.err" ]]; then
        echo "FAIL[lsp_help] lsp help should not write stderr"
        failures=$((failures + 1))
    fi
    if ! grep -q '^  feng lsp \[--stdio\]$' "$WORK/lsp_help.out"; then
        echo "FAIL[lsp_help] missing lsp help usage line"
        failures=$((failures + 1))
    fi
fi

# 0.2 lsp stdio skeleton handles initialize/shutdown/exit
lsp_initialize='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
lsp_shutdown='{"jsonrpc":"2.0","id":2,"method":"shutdown"}'
lsp_exit='{"jsonrpc":"2.0","method":"exit"}'
lsp_input="$WORK/lsp_input.bin"
{
    printf 'Content-Length: %s\r\n\r\n%s' "${#lsp_initialize}" "$lsp_initialize"
    printf 'Content-Length: %s\r\n\r\n%s' "${#lsp_shutdown}" "$lsp_shutdown"
    printf 'Content-Length: %s\r\n\r\n%s' "${#lsp_exit}" "$lsp_exit"
} >"$lsp_input"
if expect_ok "lsp_stdio" bash -lc "cat '$lsp_input' | '$FENG' lsp --stdio"; then
    if ! grep -q '"id":1' "$WORK/lsp_stdio.out"; then
        echo "FAIL[lsp_stdio] missing initialize response id"
        failures=$((failures + 1))
    fi
    if ! grep -q '"hoverProvider":true' "$WORK/lsp_stdio.out"; then
        echo "FAIL[lsp_stdio] missing hover capability"
        failures=$((failures + 1))
    fi
    if ! grep -q '"definitionProvider":true' "$WORK/lsp_stdio.out"; then
        echo "FAIL[lsp_stdio] missing definition capability"
        failures=$((failures + 1))
    fi
    if ! grep -q '"referencesProvider":true' "$WORK/lsp_stdio.out"; then
        echo "FAIL[lsp_stdio] missing references capability"
        failures=$((failures + 1))
    fi
    if ! grep -q '"renameProvider":{"prepareProvider":true}' "$WORK/lsp_stdio.out"; then
        echo "FAIL[lsp_stdio] missing rename capability"
        failures=$((failures + 1))
    fi
    if ! grep -q '"completionProvider":{"resolveProvider":true,"triggerCharacters":\["\.","_","@","a"' "$WORK/lsp_stdio.out"; then
        echo "FAIL[lsp_stdio] missing completion capability"
        failures=$((failures + 1))
    fi
    if ! grep -q '"signatureHelpProvider":{"triggerCharacters":\["(",",' "$WORK/lsp_stdio.out"; then
        echo "FAIL[lsp_stdio] missing signatureHelp capability"
        failures=$((failures + 1))
    fi
    if ! grep -q '"id":2' "$WORK/lsp_stdio.out"; then
        echo "FAIL[lsp_stdio] missing shutdown response id"
        failures=$((failures + 1))
    fi
    if ! grep -q '"result":null' "$WORK/lsp_stdio.out"; then
        echo "FAIL[lsp_stdio] missing shutdown null result"
        failures=$((failures + 1))
    fi
fi

# 0.3 lsp rejects unknown options
expect_fail "lsp_bad_arg" "$FENG" lsp --bogus || true
if ! grep -q 'unknown option: --bogus' "$WORK/lsp_bad_arg.err"; then
    echo "FAIL[lsp_bad_arg] missing unknown-option diagnostic"
    failures=$((failures + 1))
fi

# 1. happy path: full pipeline produces a runnable binary
out1="$WORK/case_full"
if expect_ok "full_pipeline" "$FENG" "$FIXTURE" --target=bin \
        --platform="$HOST_PLATFORM" --out="$out1"; then
    bin="$out1/bin/hello"
    workspace_ft="$out1/obj/symbols/feng/smoke.ft"
    public_ft="$out1/mod/feng/smoke.ft"
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
    if [[ ! -f "$workspace_ft" ]]; then
        echo "FAIL[full_pipeline] missing workspace symbol table $workspace_ft"
        failures=$((failures + 1))
    fi
    if [[ -e "$public_ft" ]]; then
        echo "FAIL[full_pipeline] private module should not emit public symbol table $public_ft"
        failures=$((failures + 1))
    fi
fi

# 2. --keep-ir preserves the intermediate C file
out2="$WORK/case_keep"
if expect_ok "keep_ir" "$FENG" "$FIXTURE" --platform="$HOST_PLATFORM" \
        --out="$out2" --keep-ir; then
    if [[ ! -f "$out2/ir/c/feng.c" ]]; then
        echo "FAIL[keep_ir] missing $out2/ir/c/feng.c"
        failures=$((failures + 1))
    fi
fi

# 3. --target=lib should produce a static archive under <out>/lib
out3="$WORK/case_lib"
if expect_ok "lib_static" "$FENG" "$FIXTURE" --target=lib \
        --platform="$HOST_PLATFORM" --out="$out3" --name=hello_lib; then
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

# 4. omitted --platform and --out default to host and ./build
default_root="$WORK/defaults"
mkdir -p "$default_root"
if ! (cd "$default_root" && "$FENG" "$FIXTURE" --target=bin) \
    >"$WORK/defaults.out" 2>"$WORK/defaults.err"; then
    echo "FAIL[defaults] direct compile with defaults failed"
    sed 's/^/  /' "$WORK/defaults.err"
    failures=$((failures + 1))
elif [[ ! -x "$default_root/build/bin/hello" ]]; then
    echo "FAIL[defaults] missing default output build/bin/hello"
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

# 7. runtime lookup failure diagnostic (FENG_RUNTIME_LIB removed).
# Previously tested the FENG_RUNTIME_LIB environment variable pointing at a
# nonexistent file. FENG_RUNTIME_LIB has been removed per the release plan
# (dev/feng-release-and-instanll.md §4.1); runtime is now located via two-step
# lookup: the single complete platform path <exe>/../lib/<os>-<arch>-<abi>/.
# A missing-runtime case requires an isolated installation layout and is covered
# by distribution manifest verification rather than this source-tree smoke test.
#
# out7="$WORK/case_rtmiss"
# if FENG_RUNTIME_LIB="$WORK/no_such_runtime.a" "$FENG" "$FIXTURE" --out="$out7" \
#        >"$WORK/rt_missing.out" 2>"$WORK/rt_missing.err"; then
#     echo "FAIL[rt_missing] expected failure with FENG_RUNTIME_LIB pointing nowhere"
#     failures=$((failures + 1))
# elif ! grep -q "FENG_RUNTIME_LIB points to" "$WORK/rt_missing.err"; then
#     echo "FAIL[rt_missing] missing FENG_RUNTIME_LIB diagnostic"
#     failures=$((failures + 1))
# fi

# 8. failing explicit Feng C compiler surfaces an error and preserves the IR
out8="$WORK/case_ccfail"
fake_cc_dir="$WORK/fake_cc"
mkdir -p "$fake_cc_dir"
cat >"$fake_cc_dir/cc" <<'INNER'
#!/usr/bin/env bash
echo "fake cc: simulated failure" >&2
exit 7
INNER
chmod +x "$fake_cc_dir/cc"
if FENG_CC="$fake_cc_dir/cc" "$FENG" "$FIXTURE" \
       --platform="$HOST_PLATFORM" --out="$out8" \
       >"$WORK/cc_fail.out" 2>"$WORK/cc_fail.err"; then
    echo "FAIL[cc_fail] expected failure when FENG_CC stub returns non-zero"
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
            --platform="$HOST_PLATFORM" \
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
expect_fail "name_empty" "$FENG" "$FIXTURE" \
    --platform="$HOST_PLATFORM" --out="$WORK/case_name" \
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
