#!/usr/bin/env python3
"""Verify Feng LSP request scheduling, cancellation, and cached definition."""

from __future__ import annotations

import argparse
import json
import pathlib
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
    """Send one request and wait for its matching response."""
    assert process.stdin is not None
    assert process.stdout is not None
    send_message(process.stdin, message)
    while True:
        response = read_message(process.stdout)
        if response.get("id") == message["id"]:
            return response


def definition_message(request_id: int, uri: str) -> dict[str, Any]:
    """Build a definition request for the call to value in the fixture."""
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": "textDocument/definition",
        "params": {
            "textDocument": {"uri": uri},
            "position": {"line": 6, "character": 8},
        },
    }


def reference_message(request_id: int, uri: str) -> dict[str, Any]:
    """Build a low-priority references request for the fixture function."""
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": "textDocument/references",
        "params": {
            "textDocument": {"uri": uri},
            "position": {"line": 6, "character": 8},
            "context": {"includeDeclaration": True},
        },
    }


def close_server(process: subprocess.Popen[bytes]) -> None:
    """Perform the normal LSP shutdown and exit sequence."""
    assert process.stdin is not None
    request(process, {"jsonrpc": "2.0", "id": 9999, "method": "shutdown", "params": None})
    send_message(process.stdin, {"jsonrpc": "2.0", "method": "exit"})
    process.stdin.close()
    process.wait(timeout=30)


def main() -> int:
    """Run definition, priority, cancellation, and recovery checks."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="build/bin/feng")
    args = parser.parse_args()
    source = (
        "module test.lsp.scheduler;\n"
        "\n"
        "func value(): i32 {\n"
        "    return 1;\n"
        "}\n"
        "func main(args: string[]) {\n"
        "    value();\n"
        "}\n"
    )
    uri = pathlib.Path("temp/lsp_scheduler.ff").resolve().as_uri()
    process = subprocess.Popen(
        [args.server, "lsp", "--stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdin is not None
    assert process.stdout is not None
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
        time.sleep(0.25)

        definition_latencies: list[float] = []
        for request_id in range(10, 40):
            started = time.perf_counter()
            response = request(process, definition_message(request_id, uri))
            definition_latencies.append((time.perf_counter() - started) * 1000.0)
            if not isinstance(response.get("result"), dict):
                raise RuntimeError(f"definition returned no location: {response}")

        for request_id in range(100, 300):
            send_message(process.stdin, reference_message(request_id, uri))
        send_message(
            process.stdin,
            {
                "jsonrpc": "2.0",
                "id": 500,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 2, "character": 15},
                },
            },
        )
        cancelled_at = time.perf_counter()
        send_message(
            process.stdin,
            {"jsonrpc": "2.0", "method": "$/cancelRequest", "params": {"id": 299}},
        )

        hover_seen = False
        cancelled_seen = False
        references_before_hover = 0
        while not (hover_seen and cancelled_seen):
            response = read_message(process.stdout)
            response_id = response.get("id")
            if response_id == 500:
                hover_seen = True
                if response.get("result") is None:
                    raise RuntimeError("prioritized Hover returned null")
            elif response_id == 299:
                cancelled_seen = True
                if response.get("error", {}).get("code") != -32800:
                    raise RuntimeError(f"cancelled request returned unexpected response: {response}")
            elif not hover_seen and isinstance(response_id, int) and 100 <= response_id < 300:
                references_before_hover += 1
        cancel_latency = (time.perf_counter() - cancelled_at) * 1000.0
        if references_before_hover >= 200:
            raise RuntimeError("highest-priority Hover remained behind all reference requests")

        invalid_source = source.replace("return 1;", "return ;")
        send_message(
            process.stdin,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": invalid_source}],
                },
            },
        )
        send_message(
            process.stdin,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 3},
                    "contentChanges": [{"text": source}],
                },
            },
        )
        recovered = request(process, definition_message(600, uri))
        if not isinstance(recovered.get("result"), dict):
            raise RuntimeError(f"definition did not recover after invalid generation: {recovered}")

        close_server(process)
        ordered = sorted(definition_latencies)
        p95 = ordered[max(0, int(len(ordered) * 0.95) - 1)]
        if p95 > 20.0:
            raise RuntimeError(f"Definition P95 {p95:.3f}ms > 20ms")
        print(
            "lsp scheduler test passed "
            f"definition_p95_ms={p95:.3f} "
            f"cancel_observed_ms={cancel_latency:.3f} "
            f"references_before_hover={references_before_hover}"
        )
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
