#!/usr/bin/env bash
# Direct-mode CLI surface checks for Phase 2 P4.
# Verifies that `feng <file> --target=bin --out=<dir>` produces the
# expected <out>/ir/c/feng.c artifact and that key error paths return
# non-zero with actionable diagnostics. Host-cc invocation is exercised
# only after P5 lands; until then we accept the known stub-warning.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FENG="$ROOT/build/bin/feng"
FIXTURE="$ROOT/test/smoke/phase1a/hello.ff"
WORK="$(mktemp -d -t feng_p4_XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

if [[ ! -x "$FENG" ]]; then
    echo "missing $FENG (run 'make cli' first)" >&2
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
        echo "FAIL[$label] expected success, exit=$?"
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

# 1. happy path: layout + IR file exists
out1="$WORK/case1"
if expect_ok "direct_bin" "$FENG" "$FIXTURE" --target=bin --out="$out1"; then
    if [[ ! -f "$out1/ir/c/feng.c" ]]; then
        echo "FAIL[direct_bin] missing $out1/ir/c/feng.c"
        failures=$((failures + 1))
    fi
    if [[ ! -d "$out1/bin" ]]; then
        echo "FAIL[direct_bin] missing $out1/bin directory"
        failures=$((failures + 1))
    fi
    if ! grep -q "int main(int argc, char \*\*argv)" "$out1/ir/c/feng.c"; then
        echo "FAIL[direct_bin] generated C lacks an executable main"
        failures=$((failures + 1))
    fi
fi

# 2. default target is bin (no --target flag)
out2="$WORK/case2"
expect_ok "default_target" "$FENG" "$FIXTURE" --out="$out2" || true
if [[ -f "$out2/ir/c/feng.c" ]]; then
    : # ok
else
    echo "FAIL[default_target] missing $out2/ir/c/feng.c"
    failures=$((failures + 1))
fi

# 3. --target=lib must be rejected by direct mode
expect_fail "lib_rejected" "$FENG" "$FIXTURE" --target=lib --out="$WORK/case3" || true
if ! grep -q "lib is not yet supported" "$WORK/lib_rejected.err"; then
    echo "FAIL[lib_rejected] missing target=lib diagnostic"
    failures=$((failures + 1))
fi

# 4. missing --out
expect_fail "no_out" "$FENG" "$FIXTURE" --target=bin || true
if ! grep -q -- "--out=<dir> is required" "$WORK/no_out.err"; then
    echo "FAIL[no_out] missing --out diagnostic"
    failures=$((failures + 1))
fi

# 5. unknown option
expect_fail "unknown_opt" "$FENG" "$FIXTURE" --out="$WORK/case5" --bogus || true
if ! grep -q "unknown option: --bogus" "$WORK/unknown_opt.err"; then
    echo "FAIL[unknown_opt] missing unknown-option diagnostic"
    failures=$((failures + 1))
fi

# 6. legacy top-level command rejected with migration hint
expect_fail "legacy_lex" "$FENG" lex "$FIXTURE" || true
if ! grep -q "use .*tool lex" "$WORK/legacy_lex.err"; then
    echo "FAIL[legacy_lex] missing migration hint"
    failures=$((failures + 1))
fi

if [[ $failures -gt 0 ]]; then
    echo "cli (P4): $failures failure(s)"
    exit 1
fi
echo "cli (P4): all checks passed"
