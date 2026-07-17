#!/usr/bin/env python3
"""Verify a failed workspace refresh keeps the last published symbol index."""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import time
from typing import Any, BinaryIO


def send_message(stream: BinaryIO, message: dict[str, Any]) -> None:
    """Write one JSON-RPC message with LSP framing."""
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    stream.write(f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii"))
    stream.write(payload)
    stream.flush()


def read_message(stream: BinaryIO) -> dict[str, Any]:
    """Read one framed JSON-RPC message."""
    content_length: int | None = None
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError("LSP server closed stdout")
        if line in (b"\r\n", b"\n"):
            break
        name, value = line.decode("ascii").split(":", 1)
        if name.lower() == "content-length":
            content_length = int(value.strip())
    if content_length is None:
        raise RuntimeError("LSP response is missing Content-Length")
    payload = stream.read(content_length)
    if len(payload) != content_length:
        raise RuntimeError("LSP response payload is truncated")
    return json.loads(payload)


def request(process: subprocess.Popen[bytes], message: dict[str, Any]) -> dict[str, Any]:
    """Send one request and return its matching response."""
    assert process.stdin is not None
    assert process.stdout is not None
    send_message(process.stdin, message)
    while True:
        response = read_message(process.stdout)
        if response.get("id") == message["id"]:
            return response


def completion_labels(
    process: subprocess.Popen[bytes], request_id: int, uri: str
) -> set[str]:
    """Return import-path completion labels from the fixture position."""
    response = request(
        process,
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 1, "character": len("import test.cache.")},
            },
        },
    )
    return {
        item["label"]
        for item in response.get("result", [])
        if isinstance(item, dict) and isinstance(item.get("label"), str)
    }


def close_server(process: subprocess.Popen[bytes]) -> None:
    """Perform the normal LSP shutdown sequence."""
    assert process.stdin is not None
    request(process, {"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": None})
    send_message(process.stdin, {"jsonrpc": "2.0", "method": "exit"})
    process.stdin.close()
    process.wait(timeout=30)


def prepare_fixture(server: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path, str]:
    """Create one packed dependency and a consumer project."""
    root = pathlib.Path("temp/lsp_cache_retention").resolve()
    if root.exists():
        shutil.rmtree(root)
    package = root / "package"
    package_source = package / "src"
    package_source.mkdir(parents=True)
    (package / "feng.fm").write_text(
        "[package]\n"
        'name: "lsp_cache_pkg"\n'
        'version: "0.1.0"\n'
        'target: "lib"\n'
        'src: "src/"\n'
        'out: "build/"\n',
        encoding="utf-8",
    )
    (package_source / "api.ff").write_text(
        "open module test.cache.pkg;\n\n"
        "open type CachedValue {\n"
        "    open let name: string;\n"
        "}\n",
        encoding="utf-8",
    )
    subprocess.run([str(server), "pack", str(package)], check=True)
    bundle = package / "build/lsp_cache_pkg-0.1.0.fb"
    if not bundle.is_file():
        raise RuntimeError(f"packed dependency was not created: {bundle}")

    consumer = root / "consumer"
    source_root = consumer / "src"
    source_root.mkdir(parents=True)
    manifest = consumer / "feng.fm"
    manifest.write_text(
        "[package]\n"
        'name: "lsp_cache_consumer"\n'
        'version: "0.1.0"\n'
        'target: "lib"\n'
        'src: "src/"\n'
        'out: "build/"\n\n'
        "[dependencies]\n"
        f'lsp_cache_pkg: "{bundle}"\n',
        encoding="utf-8",
    )
    source = (
        "module test.cache.consumer;\n"
        "import test.cache.\n\n"
        "func run(): void {}\n"
    )
    source_path = source_root / "main.ff"
    source_path.write_text(source, encoding="utf-8")
    return manifest, source_path, source


def main() -> int:
    """Publish a provider, force refresh failure, and verify retention."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="build/bin/feng")
    args = parser.parse_args()
    server = pathlib.Path(args.server).resolve()
    manifest, source_path, source = prepare_fixture(server)
    backup_manifest = manifest.with_suffix(".fm.disabled")
    uri = source_path.as_uri()
    process = subprocess.Popen(
        [str(server), "lsp", "--stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdin is not None
    try:
        request(
            process,
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}},
        )
        send_message(
            process.stdin,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": uri,
                        "languageId": "feng",
                        "version": 1,
                        "text": source,
                    }
                },
            },
        )
        time.sleep(0.2)
        before = completion_labels(process, 2, uri)
        if "pkg" not in before:
            raise RuntimeError(f"initial provider completion is missing pkg: {sorted(before)}")

        manifest.rename(backup_manifest)
        send_message(
            process.stdin,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didSave",
                "params": {"textDocument": {"uri": uri}},
            },
        )
        time.sleep(0.2)
        after = completion_labels(process, 3, uri)
        if "pkg" not in after:
            raise RuntimeError(f"failed refresh replaced the last provider: {sorted(after)}")
        close_server(process)
        print("lsp cache retention test passed label=pkg")
    finally:
        if backup_manifest.exists() and not manifest.exists():
            backup_manifest.rename(manifest)
        if process.poll() is None:
            process.kill()
            process.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
