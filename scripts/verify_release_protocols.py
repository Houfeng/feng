#!/usr/bin/env python3
"""Verify the installed Feng LSP and DAP stdio protocol entry points."""

from __future__ import annotations

import json
import subprocess
import sys
from typing import Any


def encode_message(payload: dict[str, Any]) -> bytes:
    """Encode one JSON payload using the shared Content-Length framing."""
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def parse_messages(output: bytes) -> list[dict[str, Any]]:
    """Parse all complete Content-Length framed JSON messages."""
    messages: list[dict[str, Any]] = []
    offset = 0
    while offset < len(output):
        header_end = output.find(b"\r\n\r\n", offset)
        if header_end < 0:
            raise ValueError("protocol output contains an incomplete header")
        headers = output[offset:header_end].decode("ascii").split("\r\n")
        content_length = None
        for header in headers:
            name, separator, value = header.partition(":")
            if separator and name.lower() == "content-length":
                content_length = int(value.strip())
                break
        if content_length is None:
            raise ValueError("protocol output is missing Content-Length")
        body_start = header_end + 4
        body_end = body_start + content_length
        if body_end > len(output):
            raise ValueError("protocol output contains an incomplete body")
        messages.append(json.loads(output[body_start:body_end]))
        offset = body_end
    return messages


def run_protocol(command: list[str], requests: bytes) -> list[dict[str, Any]]:
    """Run one protocol server to EOF and return its decoded responses."""
    completed = subprocess.run(
        command,
        input=requests,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=30,
    )
    if completed.returncode != 0:
        stderr = completed.stderr.decode("utf-8", errors="replace")
        raise RuntimeError(
            f"{' '.join(command)} exited with {completed.returncode}: {stderr}"
        )
    return parse_messages(completed.stdout)


def verify_lsp(feng: str) -> None:
    """Verify LSP initialize and shutdown responses."""
    requests = b"".join(
        [
            encode_message(
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {
                        "processId": None,
                        "rootUri": None,
                        "capabilities": {},
                    },
                }
            ),
            encode_message(
                {"jsonrpc": "2.0", "method": "initialized", "params": {}}
            ),
            encode_message(
                {"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": None}
            ),
            encode_message({"jsonrpc": "2.0", "method": "exit", "params": None}),
        ]
    )
    messages = run_protocol([feng, "lsp", "--stdio"], requests)
    response_ids = {
        message.get("id")
        for message in messages
        if message.get("jsonrpc") == "2.0" and "error" not in message
    }
    if 1 not in response_ids or 2 not in response_ids:
        raise RuntimeError("Feng LSP did not complete initialize and shutdown")


def verify_dap(feng: str) -> None:
    """Verify DAP initialize through bundled lldb-dap."""
    requests = encode_message(
        {
            "seq": 1,
            "type": "request",
            "command": "initialize",
            "arguments": {"adapterID": "feng"},
        }
    )
    messages = run_protocol([feng, "dap", "--stdio"], requests)
    successful_commands = {
        message.get("command")
        for message in messages
        if message.get("type") == "response" and message.get("success") is True
    }
    if "initialize" not in successful_commands:
        raise RuntimeError("Feng DAP did not complete initialize")


def main() -> int:
    """Run both installed protocol entry-point checks."""
    if len(sys.argv) != 2:
        print("usage: verify_release_protocols.py <feng-executable>", file=sys.stderr)
        return 2
    try:
        verify_lsp(sys.argv[1])
        verify_dap(sys.argv[1])
    except (OSError, subprocess.SubprocessError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print("release protocols: LSP and DAP passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
