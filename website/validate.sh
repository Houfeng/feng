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

if ! perl -CS -ne 'exit 1 if /[\x{4E00}-\x{9FFF}]/' "$INDEX_PATH" "$ROOT_DIR/app.js"; then
  fail "website still contains user-facing Chinese text"
fi

for section_id in hero features sample workflow language-map; do
  grep -q "id=\"$section_id\"" "$INDEX_PATH" || fail "missing section id: $section_id"
done

for snippet in 'href="styles.css"' 'src="app.js"' 'href="#features"' 'href="#workflow"'; do
  grep -q "$snippet" "$INDEX_PATH" || fail "missing expected reference: $snippet"
done

if grep -qE '(href|src)="\.\./' "$INDEX_PATH"; then
  fail "index.html contains a link that escapes website root"
fi

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
    case "$resolved_path" in
      "$ROOT_DIR"|"$ROOT_DIR"/*)
        ;;
      *)
        fail "link escapes website root: $target"
        ;;
    esac
    [[ -e "$resolved_path" ]] || fail "broken local reference: $target"
  done

printf 'website validation passed\n'
