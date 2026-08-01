#!/usr/bin/env python3
"""Run Feng LSP latency checks through the real stdio protocol."""

from __future__ import annotations

import argparse
import json
import pathlib
import statistics
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
    """Read one JSON-RPC message with LSP framing."""
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


def request(
    process: subprocess.Popen[bytes],
    message: dict[str, Any],
) -> tuple[dict[str, Any], float]:
    """Send one request and return its matching response and latency."""
    request_id = message["id"]
    started = time.perf_counter()
    assert process.stdin is not None
    assert process.stdout is not None
    send_message(process.stdin, message)
    while True:
        response = read_message(process.stdout)
        if response.get("id") == request_id:
            return response, (time.perf_counter() - started) * 1000.0


def percentile(samples: list[float], percentage: float) -> float:
    """Return a nearest-rank percentile for non-empty samples."""
    ordered = sorted(samples)
    index = max(0, min(len(ordered) - 1, int(len(ordered) * percentage) - 1))
    return ordered[index]


def position_for_offset(text: str, offset: int) -> dict[str, int]:
    """Convert a Python text offset to an LSP UTF-16 position."""
    before = text[:offset]
    line = before.count("\n")
    line_text = before.rsplit("\n", 1)[-1]
    character = len(line_text.encode("utf-16-le")) // 2
    return {"line": line, "character": character}


def main() -> int:
    """Run cheap Hover and dirty-document Completion latency samples."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="build/bin/feng")
    parser.add_argument("--source", default="std/std_test/src/z_main.ff")
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--no-enforce", action="store_true")
    args = parser.parse_args()

    source_path = pathlib.Path(args.source).resolve()
    source = source_path.read_text(encoding="utf-8")
    type_start = source.index("string[]")
    type_end = type_start + len("string[]")
    hover_position = position_for_offset(source, type_start + 1)
    completion_source = source[:type_start] + "str" + source[type_end:]
    completion_position = position_for_offset(completion_source, type_start + 3)
    uri = source_path.as_uri()

    process = subprocess.Popen(
        [args.server, "lsp", "--stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdin is not None
    try:
        initialize, _ = request(
            process,
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"capabilities": {}},
            },
        )
        sync_kind = initialize["result"]["capabilities"]["textDocumentSync"]["change"]
        if sync_kind != 2:
            raise RuntimeError(f"expected Incremental sync, got {sync_kind}")
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

        hover_samples: list[float] = []
        for sample in range(args.samples):
            response, elapsed = request(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 1000 + sample,
                    "method": "textDocument/hover",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": hover_position,
                    },
                },
            )
            if response.get("result") is None:
                raise RuntimeError("builtin type Hover returned null")
            hover_samples.append(elapsed)

        send_message(
            process.stdin,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [
                        {
                            "range": {
                                "start": position_for_offset(source, type_start),
                                "end": position_for_offset(source, type_end),
                            },
                            "text": "str",
                        }
                    ],
                },
            },
        )

        completion_samples: list[float] = []
        for sample in range(args.samples):
            response, elapsed = request(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 2000 + sample,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": completion_position,
                    },
                },
            )
            labels = [item.get("label") for item in response.get("result", [])]
            if "string" not in labels:
                raise RuntimeError("type Completion is missing string")
            completion_samples.append(elapsed)

        request(
            process,
            {"jsonrpc": "2.0", "id": 9999, "method": "shutdown", "params": None},
        )
        send_message(process.stdin, {"jsonrpc": "2.0", "method": "exit"})
        process.stdin.close()
        process.wait(timeout=30)

        hover_p95 = percentile(hover_samples, 0.95)
        hover_p99 = percentile(hover_samples, 0.99)
        completion_p95 = percentile(completion_samples, 0.95)
        completion_p99 = percentile(completion_samples, 0.99)
        all_p99 = percentile(hover_samples + completion_samples, 0.99)
        print(
            "hover_ms "
            f"p50={statistics.median(hover_samples):.3f} "
            f"p95={hover_p95:.3f} "
            f"p99={hover_p99:.3f} "
            f"max={max(hover_samples):.3f}"
        )
        print(
            "completion_ms "
            f"p50={statistics.median(completion_samples):.3f} "
            f"p95={completion_p95:.3f} "
            f"p99={completion_p99:.3f} "
            f"max={max(completion_samples):.3f}"
        )
        print(f"interactive_ms p99={all_p99:.3f}")
        if not args.no_enforce:
            failures: list[str] = []
            if hover_p95 > 5.0:
                failures.append(f"Hover P95 {hover_p95:.3f}ms > 5ms")
            if completion_p95 > 20.0:
                failures.append(f"Completion P95 {completion_p95:.3f}ms > 20ms")
            if all_p99 > 50.0:
                failures.append(f"interactive P99 {all_p99:.3f}ms > 50ms")
            if failures:
                raise RuntimeError("; ".join(failures))
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
