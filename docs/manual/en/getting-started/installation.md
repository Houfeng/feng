# Install Feng

This chapter helps you install the Feng toolchain and verify that the command is available.

## Supported Platforms

Official packages are currently available for:

- Apple Silicon Mac: `macos-arm64`
- x86-64 Linux: `linux-x64-gnu`
- ARM64 Linux: `linux-arm64-gnu`

Official toolchain packages are not currently available for Windows, Intel Macs, or pure musl Linux hosts.

## Quick Installation

Run this command in a terminal:

```bash
curl -fsSL https://feng-lang.com/install.sh | bash
```

The installer downloads the latest stable release for the current host. If the project has no stable release yet, it installs the RC release with the highest version instead. It installs the toolchain in `~/.feng` and adds `~/.feng/bin` to the startup file for the current shell. When installation finishes, open a new terminal or reload the startup file as instructed by the installer.

Verify the installation:

```bash
feng --version
feng --help
```

## Select a Release

To explicitly install the latest RC release, select the `rc` channel:

```bash
curl -fsSL https://feng-lang.com/install.sh | bash -s -- --channel=rc
```

The installer accepts a complete GitHub Release tag. When piping the script to Bash, pass arguments with `bash -s --`:

```bash
curl -fsSL https://feng-lang.com/install.sh | bash -s -- --version=v0.1.0
```

The version must include the `v` prefix. Running the installer again atomically replaces the existing `~/.feng` installation. If installation fails, the script restores the previous installation.

## Installed Files

`~/.feng` contains:

- `bin/feng`: the Feng command-line tool.
- `pkg/`: Feng packages included in the release.
- `include/` and `lib/`: runtime headers and target-specific runtime libraries.
- `toolchain/`: LLVM tools and target sysroots used for building and debugging.

The installer requires `curl`, `unzip`, and common POSIX commands. Based on `SHELL`, it updates the startup file for zsh, Bash, fish, ksh, or the default profile.

## Manual Installation

You can also download the archive for your host from [Feng Releases](https://github.com/Houfeng/feng/releases). Preserve the complete release directory structure after extraction and add its `bin` directory to `PATH`. Do not copy only the `feng` executable: compiling, linking, and debugging also require the runtime and toolchain from the same release.

## Next Step

Continue to [Quick Start](./quick-start.md) to create and run your first Feng program.
