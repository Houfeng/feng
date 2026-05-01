#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FENG="$ROOT/build/bin/feng"
RT_LIB="$ROOT/build/lib/libfeng_runtime.a"
FIXTURE="$ROOT/test/cli/projects/bin_hello"
LIB_FIXTURE="$ROOT/test/cli/projects/lib_hello"
INVALID_FIXTURE="$ROOT/test/cli/projects/bin_hello_invalid"
LOCAL_DEP_FIXTURE="$ROOT/test/cli/projects/local_dep_lib"
LOCAL_DEP_APP_FIXTURE="$ROOT/test/cli/projects/bin_with_local_dep"
EXPECTED="$ROOT/test/cli/projects/bin_hello.expected"
LOCAL_DEP_EXPECTED="$ROOT/test/cli/projects/bin_with_local_dep.expected"
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
INIT_BIN_FIXTURE="$WORK/init_bin"
INIT_LIB_FIXTURE="$WORK/init_lib_default"
INIT_NONEMPTY_FIXTURE="$WORK/init_nonempty"

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

if expect_ok help "$FENG" --help; then
    if ! grep -q ' init  \[<name>\] \[--target=<bin|lib>\]' "$WORK/help.err"; then
        echo "FAIL[help] missing init usage line"
        failures=$((failures + 1))
    fi
fi

mkdir -p "$INIT_BIN_FIXTURE"
if expect_ok init_bin bash -lc "cd '$INIT_BIN_FIXTURE' && '$FENG' init sample-app"; then
    if [[ ! -f "$INIT_BIN_FIXTURE/feng.fm" ]]; then
        echo "FAIL[init_bin] missing feng.fm"
        failures=$((failures + 1))
    elif ! grep -qx '\[package\]' "$INIT_BIN_FIXTURE/feng.fm"; then
        echo "FAIL[init_bin] manifest missing [package] section"
        failures=$((failures + 1))
    elif ! grep -qx 'name: "sample_app"' "$INIT_BIN_FIXTURE/feng.fm"; then
        echo "FAIL[init_bin] manifest missing explicit package name"
        failures=$((failures + 1))
    elif ! grep -qx 'target: "bin"' "$INIT_BIN_FIXTURE/feng.fm"; then
        echo "FAIL[init_bin] manifest missing target:bin"
        failures=$((failures + 1))
    fi
    if [[ ! -f "$INIT_BIN_FIXTURE/src/main.ff" ]]; then
        echo "FAIL[init_bin] missing src/main.ff"
        failures=$((failures + 1))
    elif ! grep -qx 'mod sample_app;' "$INIT_BIN_FIXTURE/src/main.ff"; then
        echo "FAIL[init_bin] starter source missing module declaration"
        failures=$((failures + 1))
    fi
fi

INIT_LIB_FIXTURE="$WORK/init-lib-default"
mkdir -p "$INIT_LIB_FIXTURE"
if expect_ok init_lib bash -lc "cd '$INIT_LIB_FIXTURE' && '$FENG' init --target=lib"; then
    expected_init_lib_name='init_lib_default'
    if [[ ! -f "$INIT_LIB_FIXTURE/feng.fm" ]]; then
        echo "FAIL[init_lib] missing feng.fm"
        failures=$((failures + 1))
    elif ! grep -qx '\[package\]' "$INIT_LIB_FIXTURE/feng.fm"; then
        echo "FAIL[init_lib] manifest missing [package] section"
        failures=$((failures + 1))
    elif ! grep -qx "name: \"$expected_init_lib_name\"" "$INIT_LIB_FIXTURE/feng.fm"; then
        echo "FAIL[init_lib] manifest missing derived package name"
        failures=$((failures + 1))
    elif ! grep -qx 'target: "lib"' "$INIT_LIB_FIXTURE/feng.fm"; then
        echo "FAIL[init_lib] manifest missing target:lib"
        failures=$((failures + 1))
    fi
    if [[ ! -f "$INIT_LIB_FIXTURE/src/lib.ff" ]]; then
        echo "FAIL[init_lib] missing src/lib.ff"
        failures=$((failures + 1))
    elif ! grep -qx "mod $expected_init_lib_name;" "$INIT_LIB_FIXTURE/src/lib.ff"; then
        echo "FAIL[init_lib] starter library source missing module declaration"
        failures=$((failures + 1))
    fi
fi

mkdir -p "$INIT_NONEMPTY_FIXTURE"
printf 'occupied\n' >"$INIT_NONEMPTY_FIXTURE/README.md"
expect_fail init_nonempty bash -lc "cd '$INIT_NONEMPTY_FIXTURE' && '$FENG' init blocked" || true
if ! grep -q 'current directory is not empty' "$WORK/init_nonempty.err"; then
    echo "FAIL[init_nonempty] missing non-empty directory diagnostic"
    failures=$((failures + 1))
fi
if [[ -e "$INIT_NONEMPTY_FIXTURE/feng.fm" || -d "$INIT_NONEMPTY_FIXTURE/src" ]]; then
    echo "FAIL[init_nonempty] init should not create files in a non-empty directory"
    failures=$((failures + 1))
fi

rm -rf "$FIXTURE/build"
rm -rf "$LIB_FIXTURE/build"
rm -rf "$LOCAL_DEP_FIXTURE/build"
rm -rf "$LOCAL_DEP_APP_FIXTURE/build"

if expect_ok build "$FENG" build "$FIXTURE"; then
    bin="$FIXTURE/build/bin/hello_project"
    workspace_ft="$FIXTURE/build/obj/symbols/feng/cli/project.ft"
    public_ft="$FIXTURE/build/mod/feng/cli/project.ft"
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
    if [[ ! -f "$workspace_ft" ]]; then
        echo "FAIL[build] missing workspace symbol table $workspace_ft"
        failures=$((failures + 1))
    fi
    if [[ -e "$public_ft" ]]; then
        echo "FAIL[build] private module should not emit public symbol table $public_ft"
        failures=$((failures + 1))
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

if expect_ok build_local_dep_app "$FENG" build "$LOCAL_DEP_APP_FIXTURE"; then
    bin="$LOCAL_DEP_APP_FIXTURE/build/bin/local_dep_app"
    dep_bundle="$LOCAL_DEP_FIXTURE/build/local_dep-0.1.0.fb"
    if [[ ! -x "$bin" ]]; then
        echo "FAIL[build_local_dep_app] missing executable $bin"
        failures=$((failures + 1))
    else
        actual="$($bin)"
        expected_text="$(cat "$LOCAL_DEP_EXPECTED")"
        if [[ "$actual" != "$expected_text" ]]; then
            echo "FAIL[build_local_dep_app] stdout mismatch"
            echo "  expected: $expected_text"
            echo "  actual:   $actual"
            failures=$((failures + 1))
        fi
    fi
    if [[ ! -f "$dep_bundle" ]]; then
        echo "FAIL[build_local_dep_app] missing local dependency bundle $dep_bundle"
        failures=$((failures + 1))
    fi
fi

if expect_ok run_local_dep_app "$FENG" run "$LOCAL_DEP_APP_FIXTURE"; then
    actual="$(cat "$WORK/run_local_dep_app.out")"
    expected_text="$(cat "$LOCAL_DEP_EXPECTED")"
    if [[ "$actual" != "$expected_text" ]]; then
        echo "FAIL[run_local_dep_app] stdout mismatch"
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
    workspace_ft="$LIB_FIXTURE/build/obj/symbols/feng/cli/project/lib.ft"
    public_ft="$LIB_FIXTURE/build/mod/feng/cli/project/lib.ft"
    if [[ ! -f "$lib" ]]; then
        echo "FAIL[build_lib] missing archive $lib"
        failures=$((failures + 1))
    elif ! ar -t "$lib" | grep -q '^feng.o$'; then
        echo "FAIL[build_lib] archive does not contain feng.o"
        failures=$((failures + 1))
    fi
    if [[ ! -f "$workspace_ft" ]]; then
        echo "FAIL[build_lib] missing workspace symbol table $workspace_ft"
        failures=$((failures + 1))
    fi
    if [[ ! -f "$public_ft" ]]; then
        echo "FAIL[build_lib] missing public symbol table $public_ft"
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
            if ! grep -qx 'mod/feng/cli/project/lib.ft' "$WORK/pack_lib.entries"; then
                echo "FAIL[pack_lib] bundle missing public module .ft"
                failures=$((failures + 1))
            fi
            if ! grep -qx 'mod/feng/' "$WORK/pack_lib.entries"; then
                echo "FAIL[pack_lib] bundle missing intermediate module directory"
                failures=$((failures + 1))
            fi
            if unzip -p "$package" mod/feng/cli/project/lib.ft >"$WORK/pack_lib.ft" 2>"$WORK/pack_lib.ft.err"; then
                magic="$(head -c 4 "$WORK/pack_lib.ft")"
                if [[ "$magic" != "FST1" ]]; then
                    echo "FAIL[pack_lib] packaged .ft has wrong magic: '$magic'"
                    failures=$((failures + 1))
                fi
                disk_ft="$LIB_FIXTURE/build/mod/feng/cli/project/lib.ft"
                if [[ ! -f "$disk_ft" ]]; then
                    echo "FAIL[pack_lib] missing on-disk source .ft for byte-equality check"
                    failures=$((failures + 1))
                elif ! cmp -s "$WORK/pack_lib.ft" "$disk_ft"; then
                    echo "FAIL[pack_lib] packaged .ft bytes differ from on-disk source"
                    failures=$((failures + 1))
                fi
            else
                echo "FAIL[pack_lib] failed to extract packaged .ft"
                sed 's/^/  /' "$WORK/pack_lib.ft.err"
                failures=$((failures + 1))
            fi
        fi

        if ! unzip -p "$package" feng.fm >"$WORK/pack_lib.manifest" 2>"$WORK/pack_lib.manifest.err"; then
            echo "FAIL[pack_lib] failed to read packaged feng.fm"
            sed 's/^/  /' "$WORK/pack_lib.manifest.err"
            failures=$((failures + 1))
        else
            if ! grep -qx '\[package\]' "$WORK/pack_lib.manifest"; then
                echo "FAIL[pack_lib] packaged manifest missing [package] section"
                failures=$((failures + 1))
            fi
            if ! grep -qx 'name: "hello_library"' "$WORK/pack_lib.manifest"; then
                echo "FAIL[pack_lib] packaged manifest missing name"
                failures=$((failures + 1))
            fi
            if ! grep -qx 'version: "0.1.0"' "$WORK/pack_lib.manifest"; then
                echo "FAIL[pack_lib] packaged manifest missing version"
                failures=$((failures + 1))
            fi
            if ! grep -qx "arch: \"$HOST_TARGET\"" "$WORK/pack_lib.manifest"; then
                echo "FAIL[pack_lib] packaged manifest missing host arch"
                failures=$((failures + 1))
            fi
            if ! grep -qx 'abi: "feng"' "$WORK/pack_lib.manifest"; then
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