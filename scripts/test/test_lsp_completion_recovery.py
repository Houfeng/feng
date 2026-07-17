#!/usr/bin/env python3
"""Verify member completion recovers after deleting and retyping a dot."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import time
from typing import Any, BinaryIO


def send_message(stream: BinaryIO, message: dict[str, Any]) -> None:
    """Write one JSON-RPC message using LSP framing."""
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    stream.write(f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii"))
    stream.write(payload)
    stream.flush()


def read_message(stream: BinaryIO) -> dict[str, Any]:
    """Read one framed LSP message."""
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
    """Send a request and wait for its matching response."""
    assert process.stdin is not None
    assert process.stdout is not None
    send_message(process.stdin, message)
    while True:
        response = read_message(process.stdout)
        if response.get("id") == message["id"]:
            return response


def position_for_offset(text: str, offset: int) -> dict[str, int]:
    """Convert a Python string offset to an LSP UTF-16 position."""
    before = text[:offset]
    line = before.count("\n")
    line_text = before.rsplit("\n", 1)[-1]
    return {"line": line, "character": len(line_text.encode("utf-16-le")) // 2}


def completion_request(
    process: subprocess.Popen[bytes],
    request_id: int,
    uri: str,
    position: dict[str, int],
) -> set[str]:
    """Request completion and return all string labels."""
    response = request(
        process,
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "textDocument/completion",
            "params": {"textDocument": {"uri": uri}, "position": position},
        },
    )
    return {
        item["label"]
        for item in response.get("result", [])
        if isinstance(item, dict) and isinstance(item.get("label"), str)
    }


def change_dot(
    process: subprocess.Popen[bytes],
    uri: str,
    version: int,
    position: dict[str, int],
    insert: bool,
) -> None:
    """Insert or delete the member-access dot with an incremental edit."""
    assert process.stdin is not None
    end = dict(position)
    if not insert:
        end["character"] += 1
    send_message(
        process.stdin,
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [
                    {
                        "range": {"start": position, "end": end},
                        "text": "." if insert else "",
                    }
                ],
            },
        },
    )


def cold_completion_labels(
    server: str,
    uri: str,
    text: str,
    position: dict[str, int],
) -> set[str]:
    """Return member labels from a newly started LSP process."""
    process = subprocess.Popen(
        [server, "lsp", "--stdio"],
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
                        "text": text,
                    }
                },
            },
        )
        labels = completion_request(process, 2, uri, position)
        request(process, {"jsonrpc": "2.0", "id": 9, "method": "shutdown", "params": None})
        send_message(process.stdin, {"jsonrpc": "2.0", "method": "exit"})
        process.stdin.close()
        process.wait(timeout=30)
        return labels
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def main() -> int:
    """Run the valid-to-incomplete-to-recovered completion sequence."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="build/bin/feng")
    args = parser.parse_args()

    valid_source = (
        "module test.lsp.completion_recovery;\n"
        "\n"
        "type User {\n"
        "    let name: string;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let user: User = User { name: \"feng\" };\n"
        "    user;\n"
        "}\n"
    )
    expression_offset = valid_source.index("    user;", valid_source.index("func main")) + len("    user")
    edit_position = position_for_offset(valid_source, expression_offset)
    completion_position = dict(edit_position)
    completion_position["character"] += 1
    uri = pathlib.Path("temp/lsp_completion_recovery.ff").resolve().as_uri()

    process = subprocess.Popen(
        [args.server, "lsp", "--stdio"],
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
                        "text": valid_source,
                    }
                },
            },
        )
        time.sleep(0.05)

        change_dot(process, uri, 2, edit_position, True)
        first_labels = completion_request(process, 2, uri, completion_position)
        change_dot(process, uri, 3, edit_position, False)
        time.sleep(0.05)
        change_dot(process, uri, 4, edit_position, True)
        second_labels = completion_request(process, 3, uri, completion_position)

        incomplete_source = valid_source[:expression_offset] + "." + valid_source[expression_offset:]
        send_message(
            process.stdin,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 5},
                    "contentChanges": [{"text": incomplete_source}],
                },
            },
        )
        full_change_labels = completion_request(process, 4, uri, completion_position)

        if "name" not in first_labels:
            raise RuntimeError(f"first member completion is missing name: {sorted(first_labels)}")
        if second_labels != first_labels:
            raise RuntimeError(
                "member completion did not recover after retyping dot: "
                f"first={sorted(first_labels)}, second={sorted(second_labels)}, "
                f"full_change={sorted(full_change_labels)}"
            )
        if full_change_labels != first_labels:
            raise RuntimeError(
                "incremental and full change produced different member completion: "
                f"incremental={sorted(second_labels)}, full={sorted(full_change_labels)}"
            )

        request(process, {"jsonrpc": "2.0", "id": 9, "method": "shutdown", "params": None})
        send_message(process.stdin, {"jsonrpc": "2.0", "method": "exit"})
        process.stdin.close()
        process.wait(timeout=30)

        cold_labels = cold_completion_labels(
            args.server,
            uri,
            incomplete_source,
            completion_position,
        )
        if cold_labels != first_labels:
            raise RuntimeError(
                "recovered and cold-start member completion differ: "
                f"recovered={sorted(second_labels)}, cold={sorted(cold_labels)}"
            )
        print("lsp completion recovery test passed")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
