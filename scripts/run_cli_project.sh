#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FENG="$ROOT/build/bin/feng"
RT_LIB="$ROOT/build/lib/libfeng_runtime.a"
FIXTURE="$ROOT/test/cli/projects/bin_hello"
LIB_FIXTURE="$ROOT/test/cli/projects/lib_hello"
INVALID_FIXTURE="$ROOT/test/cli/projects/bin_hello_invalid"
EXPECTED="$ROOT/test/cli/projects/bin_hello.expected"
WORK="$(mktemp -d -t feng_cli_project_XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

if [[ ! -x "$FENG" ]]; then
    echo "missing $FENG (run 'make cli' first)" >&2
    exit 2
fi
if [[ ! -f "$RT_LIB" ]]; then
    echo "missing $RT_LIB (run 'make runtime' first)" >&2
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

rm -rf "$FIXTURE/build"
rm -rf "$LIB_FIXTURE/build"

if expect_ok build "$FENG" build "$FIXTURE"; then
    bin="$FIXTURE/build/bin/hello_project"
    if [[ ! -x "$bin" ]]; then
        echo "FAIL[build] missing executable $bin"
        failures=$((failures + 1))
    else
        actual="$($bin)"
        expected_text="$(cat "$EXPECTED")"
        if [[ "$actual" != "$expected_text" ]]; then
            echo "FAIL[build] stdout mismatch"
            echo "  expected: $expected_text"
            echo "  actual:   $actual"
            failures=$((failures + 1))
        fi
    fi
fi

if expect_ok check_json "$FENG" check "$FIXTURE" --format=json; then
    if ! grep -q '^\[' "$WORK/check_json.out"; then
        echo "FAIL[check_json] missing JSON array start"
        failures=$((failures + 1))
    fi
    if grep -q '"severity"' "$WORK/check_json.out"; then
        echo "FAIL[check_json] expected empty diagnostic array"
        failures=$((failures + 1))
    fi
fi

if expect_ok run "$FENG" run "$FIXTURE"; then
    actual="$(cat "$WORK/run.out")"
    expected_text="$(cat "$EXPECTED")"
    if [[ "$actual" != "$expected_text" ]]; then
        echo "FAIL[run] stdout mismatch"
        echo "  expected: $expected_text"
        echo "  actual:   $actual"
        failures=$((failures + 1))
    fi
fi

if expect_ok clean "$FENG" clean "$FIXTURE"; then
    if [[ -e "$FIXTURE/build" ]]; then
        echo "FAIL[clean] expected $FIXTURE/build to be removed"
        failures=$((failures + 1))
    fi
fi

if expect_ok default_path bash -lc "cd '$FIXTURE' && '$FENG' build"; then
    if [[ ! -x "$FIXTURE/build/bin/hello_project" ]]; then
        echo "FAIL[default_path] missing rebuilt executable"
        failures=$((failures + 1))
    fi
fi

if expect_ok build_lib "$FENG" build "$LIB_FIXTURE"; then
    lib="$LIB_FIXTURE/build/lib/libhello_library.a"
    if [[ ! -f "$lib" ]]; then
        echo "FAIL[build_lib] missing archive $lib"
        failures=$((failures + 1))
    elif ! ar -t "$lib" | grep -q '^feng.o$'; then
        echo "FAIL[build_lib] archive does not contain feng.o"
        failures=$((failures + 1))
    fi
fi

if expect_ok clean_lib "$FENG" clean "$LIB_FIXTURE"; then
    if [[ -e "$LIB_FIXTURE/build" ]]; then
        echo "FAIL[clean_lib] expected $LIB_FIXTURE/build to be removed"
        failures=$((failures + 1))
    fi
fi

expect_fail invalid_manifest "$FENG" build "$INVALID_FIXTURE" || true
if ! grep -q 'unsupported manifest field' "$WORK/invalid_manifest.err"; then
    echo "FAIL[invalid_manifest] missing manifest diagnostic"
    failures=$((failures + 1))
fi

expect_fail pack_pending "$FENG" pack "$FIXTURE" || true
if ! grep -q 'not yet available' "$WORK/pack_pending.err"; then
    echo "FAIL[pack_pending] missing pack pending diagnostic"
    failures=$((failures + 1))
fi

if [[ $failures -gt 0 ]]; then
    echo "cli (project mode): $failures failure(s)"
    exit 1
fi
echo "cli (project mode): all checks passed"