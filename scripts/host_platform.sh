#!/usr/bin/env bash

# Print the native Feng os-arch identifier used by package artefacts.
feng_detect_host_target() {
    local os arch

    case "$(uname -s)" in
        Darwin) os="macos" ;;
        Linux) os="linux" ;;
        MINGW*|MSYS*|CYGWIN*) os="windows" ;;
        *)
            echo "unsupported host OS: $(uname -s)" >&2
            return 1
            ;;
    esac
    case "$(uname -m)" in
        arm64|aarch64) arch="arm64" ;;
        x86_64|amd64) arch="x64" ;;
        *)
            echo "unsupported host architecture: $(uname -m)" >&2
            return 1
            ;;
    esac
    printf '%s-%s\n' "$os" "$arch"
}

# Print the complete native Feng platform identifier used by runtime paths.
feng_detect_host_platform() {
    local target

    target="$(feng_detect_host_target)" || return 1
    case "$target" in
        linux-*) printf '%s-gnu\n' "$target" ;;
        *) printf '%s\n' "$target" ;;
    esac
}
