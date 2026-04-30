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

detect_host_target() {
    local os arch

    case "$(uname -s)" in
        Darwin)
            os="macos"
            ;;
        Linux)
            os="linux"
            ;;
        MINGW*|MSYS*|CYGWIN*)
            os="windows"
            ;;
        *)
            echo "unsupported host OS for project smoke: $(uname -s)" >&2
            exit 2
            ;;
    esac

    case "$(uname -m)" in
        arm64|aarch64)
            arch="arm64"
            ;;
        x86_64|amd64)
            arch="x64"
            ;;
        *)
            echo "unsupported host architecture for project smoke: $(uname -m)" >&2
            exit 2
            ;;
    esac

    printf '%s-%s\n' "$os" "$arch"
}

HOST_TARGET="$(detect_host_target)"

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

if expect_ok pack_lib "$FENG" pack "$LIB_FIXTURE"; then
    package="$LIB_FIXTURE/build/hello_library-0.1.0.fb"
    if [[ ! -f "$package" ]]; then
        echo "FAIL[pack_lib] missing bundle $package"
        failures=$((failures + 1))
    else
        if ! unzip -Z1 "$package" >"$WORK/pack_lib.entries" 2>"$WORK/pack_lib.entries.err"; then
            echo "FAIL[pack_lib] failed to list bundle entries"
            sed 's/^/  /' "$WORK/pack_lib.entries.err"
            failures=$((failures + 1))
        else
            if ! grep -qx 'feng.fm' "$WORK/pack_lib.entries"; then
                echo "FAIL[pack_lib] bundle missing feng.fm"
                failures=$((failures + 1))
            fi
            if ! grep -qx 'mod/' "$WORK/pack_lib.entries"; then
                echo "FAIL[pack_lib] bundle missing mod/ directory"
                failures=$((failures + 1))
            fi
            if ! grep -qx "lib/$HOST_TARGET/" "$WORK/pack_lib.entries"; then
                echo "FAIL[pack_lib] bundle missing host library directory"
                failures=$((failures + 1))
            fi
            if ! grep -qx "lib/$HOST_TARGET/libhello_library.a" "$WORK/pack_lib.entries"; then
                echo "FAIL[pack_lib] bundle missing host library entry"
                failures=$((failures + 1))
            fi
        fi

        if ! unzip -p "$package" feng.fm >"$WORK/pack_lib.manifest" 2>"$WORK/pack_lib.manifest.err"; then
            echo "FAIL[pack_lib] failed to read packaged feng.fm"
            sed 's/^/  /' "$WORK/pack_lib.manifest.err"
            failures=$((failures + 1))
        else
            if ! grep -qx 'name:hello_library' "$WORK/pack_lib.manifest"; then
                echo "FAIL[pack_lib] packaged manifest missing name"
                failures=$((failures + 1))
            fi
            if ! grep -qx 'version:0.1.0' "$WORK/pack_lib.manifest"; then
                echo "FAIL[pack_lib] packaged manifest missing version"
                failures=$((failures + 1))
            fi
            if ! grep -qx "arch:$HOST_TARGET" "$WORK/pack_lib.manifest"; then
                echo "FAIL[pack_lib] packaged manifest missing host arch"
                failures=$((failures + 1))
            fi
            if ! grep -qx 'abi:feng' "$WORK/pack_lib.manifest"; then
                echo "FAIL[pack_lib] packaged manifest missing abi:feng"
                failures=$((failures + 1))
            fi
        fi

        if unzip -p "$package" "lib/$HOST_TARGET/libhello_library.a" >"$WORK/pack_lib.a" 2>"$WORK/pack_lib.a.err"; then
            if ! ar -t "$WORK/pack_lib.a" | grep -q '^feng.o$'; then
                echo "FAIL[pack_lib] packaged archive does not contain feng.o"
                failures=$((failures + 1))
            fi
        else
            echo "FAIL[pack_lib] failed to extract packaged archive"
            sed 's/^/  /' "$WORK/pack_lib.a.err"
            failures=$((failures + 1))
        fi
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

expect_fail pack_requires_lib "$FENG" pack "$FIXTURE" || true
if ! grep -q 'requires `target:lib`' "$WORK/pack_requires_lib.err"; then
    echo "FAIL[pack_requires_lib] missing target=lib diagnostic"
    failures=$((failures + 1))
fi


if [[ $failures -gt 0 ]]; then
    echo "cli (project mode): $failures failure(s)"
    exit 1
fi
echo "cli (project mode): all checks passed"