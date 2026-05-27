#!/usr/bin/env bash
set -euo pipefail

# Current dir
CURRENT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# VSCode Extension
VSCODE_DIR="$CURRENT_DIR/../editors/feng-vscode"

if [[ ! -d "$VSCODE_DIR" ]]; then
	echo "error: VS Code extension directory not found: $VSCODE_DIR" >&2
	exit 1
fi

if ! command -v npm >/dev/null 2>&1; then
	echo "error: npm is not installed or not in PATH" >&2
	exit 1
fi

cd "$VSCODE_DIR"
npm run pack

# Back to current dir
cd "$CURRENT_DIR"