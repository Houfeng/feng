#!/usr/bin/env bash
# Exercise the real Make and CI checks using isolated compiler-version fixtures.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
make_command=$(command -v make)
host_os=$(uname -s)
mkdir -p "$root/temp"
work=$(mktemp -d "$root/temp/host-clang-version.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/build" "$work/src/cli" "$work/sdk"
cp "$root/Makefile" "$root/VERSION" "$work/"
export FENG_TEST_TOOLS="$work" FENG_TEST_OS=Darwin
export FENG_TEST_CLANG_VERSION=23.1.3 FENG_TEST_CC_VERSION=23.1.3
export FENG_TEST_CLANG_STATUS=0 FENG_TEST_CC_STATUS=0
checks=0

cat > "$work/bin/clang" <<'SH'
#!/bin/sh
[ "$1" = -dumpversion ] || exit 64
case "${0##*/}" in
  clang) printf '%s\n' "$FENG_TEST_CLANG_VERSION"; exit "$FENG_TEST_CLANG_STATUS" ;;
  cc) printf '%s\n' "$FENG_TEST_CC_VERSION"; exit "$FENG_TEST_CC_STATUS" ;;
esac
exit 64
SH
cp "$work/bin/clang" "$work/bin/cc"
cat > "$work/bin/uname" <<'SH'
#!/bin/sh
case "$1" in -s) echo "$FENG_TEST_OS" ;; -m) echo arm64 ;; *) exit 64 ;; esac
SH
cat > "$work/bin/brew" <<'SH'
#!/bin/sh
[ "$1" = --prefix ] && [ "$2" = llvm@23 ] || exit 64
echo "$FENG_TEST_TOOLS"
SH
cat > "$work/bin/sw_vers" <<'SH'
#!/bin/sh
echo 26.0
SH
cat > "$work/bin/xcodebuild" <<'SH'
#!/bin/sh
echo 'Xcode 26.3'
SH
cat > "$work/bin/xcrun" <<'SH'
#!/bin/sh
case "$3" in
  --show-sdk-version) echo 26.2 ;;
  --show-sdk-path) echo "$FENG_TEST_TOOLS/sdk" ;;
  *) exit 64 ;;
esac
SH
# These tools are only checked for presence by the environment validator.
for tool in file node timeout unzip zip ar ld.lld; do
  printf '#!/bin/sh\nexit 64\n' > "$work/bin/$tool"
done
chmod +x "$work/bin/"*
export PATH="$work/bin:$PATH"
cd "$work"

# Require the expected status and diagnostic from a complete public entry point.
expect_result() {
  local expected="$1" diagnostic="$2" status=0
  shift 2
  "$@" > "$work/result.log" 2>&1 || status=$?
  if { [[ "$expected" == pass ]] && [[ "$status" != 0 ]]; } ||
     { [[ "$expected" == fail ]] && [[ "$status" == 0 ]]; }; then
    cat "$work/result.log" >&2
    echo "unexpected result ($status): $*" >&2
    exit 1
  fi
  if [[ -n "$diagnostic" ]]; then
    grep -F -- "$diagnostic" "$work/result.log" >/dev/null || {
      cat "$work/result.log" >&2
      echo "missing diagnostic: $diagnostic" >&2
      exit 1
    }
  fi
  checks=$((checks + 1))
}

# Check both compiler names independently, including failed and malformed output.
version_matrix() {
  local compiler="$1" expected version diagnostic
  shift
  while IFS='|' read -r expected version; do
    export FENG_TEST_CLANG_VERSION=23.1.3 FENG_TEST_CC_VERSION=23.1.3
    if [[ "$compiler" == clang ]]; then
      export FENG_TEST_CLANG_VERSION="$version"
    else
      export FENG_TEST_CC_VERSION="$version"
    fi
    diagnostic=""
    if [[ "$expected" == fail ]]; then diagnostic='>= 23.1.3 and < 24.0.0'; fi
    expect_result "$expected" "$diagnostic" "$@"
    if [[ "$expected" == fail ]]; then
      grep -F -- "$work/bin/$compiler" "$work/result.log" >/dev/null
    fi
  done <<'VERSIONS'
pass|23.1.3
pass|23.1.4
pass|23.1.10
pass|23.2.0
fail|23.1.2
fail|23.0.99
fail|22.9.9
fail|24.0.0
fail|23.1
fail|23.1.3git
fail|23.1.3.1
fail|garbage
fail|
VERSIONS
  export FENG_TEST_CLANG_VERSION=23.1.3 FENG_TEST_CC_VERSION=23.1.3
  if [[ "$compiler" == clang ]]; then
    export FENG_TEST_CLANG_VERSION=$'23.1.3\n24.0.0'
  else
    export FENG_TEST_CC_VERSION=$'23.1.3\n24.0.0'
  fi
  expect_result fail '>= 23.1.3 and < 24.0.0' "$@"
  export FENG_TEST_CLANG_VERSION=23.1.3 FENG_TEST_CC_VERSION=23.1.3
  if [[ "$compiler" == clang ]]; then
    export FENG_TEST_CLANG_STATUS=1
  else
    export FENG_TEST_CC_STATUS=1
  fi
  expect_result fail "cannot read $compiler version" "$@"
  export FENG_TEST_CLANG_STATUS=0 FENG_TEST_CC_STATUS=0
}

for compiler in clang cc; do
  version_matrix "$compiler" "$make_command" --no-print-directory "check-$compiler"
  version_matrix "$compiler" bash "$root/scripts/release_ci_environment.sh" --platform=macos-arm64
done
if [[ "$host_os" == Linux ]]; then
  export FENG_TEST_OS=Linux
  for compiler in clang cc; do
    version_matrix "$compiler" bash "$root/scripts/release_ci_environment.sh" --platform=linux-arm64-gnu
  done
fi

# A rejected compiler must stop every regression entry point before make clean.
printf 'preserve\n' > "$work/build/sentinel"
for compiler in clang cc; do
  export FENG_TEST_CLANG_VERSION=23.1.3 FENG_TEST_CC_VERSION=23.1.3
  if [[ "$compiler" == clang ]]; then
    export FENG_TEST_CLANG_VERSION=23.1.2
  else
    export FENG_TEST_CC_VERSION=24.0.0
  fi
  for target in test test-normal test-sanitize; do
    expect_result fail '>= 23.1.3 and < 24.0.0' "$make_command" --no-print-directory "$target"
    [[ $(cat "$work/build/sentinel") == preserve ]]
  done
done
echo "host Clang versions: $checks Make/CI checks passed"
