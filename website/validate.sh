#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
INDEX_PATH="$ROOT_DIR/index.html"

fail() {
  printf 'website validation failed: %s\n' "$1" >&2
  exit 1
}

require_file() {
  local path="$1"
  [[ -f "$path" ]] || fail "missing file: $path"
}

resolve_target() {
  local target="$1"
  local target_dir
  local target_name

  target_dir="$(dirname "$target")"
  target_name="$(basename "$target")"

  (
    cd "$ROOT_DIR"
    cd "$target_dir"
    printf '%s/%s\n' "$PWD" "$target_name"
  )
}

require_file "$INDEX_PATH"
require_file "$ROOT_DIR/styles.css"
require_file "$ROOT_DIR/app.js"

for section_id in hero pillars sample workflow docs-map status; do
  grep -q "id=\"$section_id\"" "$INDEX_PATH" || fail "missing section id: $section_id"
done

for snippet in 'href="styles.css"' 'src="app.js"' '../docs/feng-language.md' '../docs/feng-cli.md' '../docs/feng-build.md' '../docs/feng-package.md' '../docs/feng-interop.md' '../docs/feng-lifetime.md' '../examples/src/hello_world.ff' '../examples/feng.fm'; do
  grep -q "$snippet" "$INDEX_PATH" || fail "missing expected reference: $snippet"
done

grep -Eo '(href|src)="[^"]+"' "$INDEX_PATH" |
  sed -E 's/^[^=]+=\"([^\"]+)\"$/\1/' |
  while IFS= read -r target; do
    [[ -n "$target" ]] || continue
    case "$target" in
      http:*|https:*|mailto:*|tel:*|\#*)
        continue
        ;;
    esac

    resolved_path="$(resolve_target "$target")"
    [[ -e "$resolved_path" ]] || fail "broken local reference: $target"
  done

printf 'website validation passed\n'
