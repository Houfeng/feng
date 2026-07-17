#!/usr/bin/env python3
"""Verify chained member completion against the TUI source dependency index."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import time

from test_lsp_completion_recovery import (
    completion_request,
    position_for_offset,
    request,
    send_message,
)


def main() -> int:
    """Open an incomplete `app.screen.` edit and wait for indexed members."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="build/bin/feng")
    args = parser.parse_args()

    path = pathlib.Path("examples/tui_demo/src/main.ff").resolve()
    valid_source = path.read_text(encoding="utf-8")
    next_statement = "    app.screen.buffer().clear();\n"
    insertion_offset = valid_source.index(next_statement)
    insertion = "    app.screen.\n"
    incomplete_source = (
        valid_source[:insertion_offset]
        + insertion
        + valid_source[insertion_offset:]
    )
    completion_position = position_for_offset(
        incomplete_source,
        insertion_offset + len("    app.screen."),
    )
    uri = path.as_uri()
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
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"capabilities": {}},
            },
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
                        "text": incomplete_source,
                    }
                },
            },
        )

        labels: set[str] = set()
        request_id = 1
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            request_id += 1
            labels = completion_request(
                process,
                request_id,
                uri,
                completion_position,
            )
            if labels:
                break
            time.sleep(0.05)

        required = {"buffer", "size", "resize"}
        unrelated = {"Action", "args", "break", "catch", "continue", "defer"}
        if not required.issubset(labels):
            raise RuntimeError(
                "chained completion is missing Screen members: "
                f"required={sorted(required)}, actual={sorted(labels)}"
            )
        leaked = labels.intersection(unrelated)
        if leaked:
            raise RuntimeError(
                "chained completion leaked non-member candidates: "
                f"leaked={sorted(leaked)}, actual={sorted(labels)}"
            )

        request(
            process,
            {"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": None},
        )
        send_message(process.stdin, {"jsonrpc": "2.0", "method": "exit"})
        process.stdin.close()
        process.wait(timeout=30)
        print("lsp chained member completion test passed")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
