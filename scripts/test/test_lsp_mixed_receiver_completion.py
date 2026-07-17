#!/usr/bin/env python3
"""Verify member completion for mixed member/call/index receiver chains."""

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


def completion_labels(
    server: str,
    path: pathlib.Path,
    source: str,
    marker: str,
    wait_seconds: float = 5.0,
) -> set[str]:
    """Open one incomplete source and return its first non-empty completion."""
    offset = source.index(marker) + len(marker)
    position = position_for_offset(source, offset)
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
                        "uri": path.resolve().as_uri(),
                        "languageId": "feng",
                        "version": 1,
                        "text": source,
                    }
                },
            },
        )
        labels: set[str] = set()
        request_id = 1
        deadline = time.monotonic() + wait_seconds
        while time.monotonic() < deadline:
            request_id += 1
            labels = completion_request(
                process,
                request_id,
                path.resolve().as_uri(),
                position,
            )
            if labels:
                break
            time.sleep(0.05)
        request(
            process,
            {"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": None},
        )
        send_message(process.stdin, {"jsonrpc": "2.0", "method": "exit"})
        process.stdin.close()
        process.wait(timeout=30)
        return labels
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def require_members(labels: set[str], required: set[str], case: str) -> None:
    """Require owner members and reject unrelated fallback candidates."""
    unrelated = {"Action", "args", "break", "catch", "continue", "defer"}
    if not required.issubset(labels):
        raise RuntimeError(
            f"{case} is missing members: required={sorted(required)}, "
            f"actual={sorted(labels)}"
        )
    leaked = labels.intersection(unrelated)
    if leaked:
        raise RuntimeError(
            f"{case} leaked non-members: leaked={sorted(leaked)}, "
            f"actual={sorted(labels)}"
        )


def main() -> int:
    """Run real-project call-chain and synthetic mixed-chain regressions."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="build/bin/feng")
    args = parser.parse_args()

    tui_path = pathlib.Path("examples/tui_demo/src/main.ff")
    tui_source = tui_path.read_text(encoding="utf-8")
    insertion_point = "    app.screen.buffer().clear();"
    insertion_offset = tui_source.index(insertion_point)
    tui_marker = "    app.screen.buffer()."
    tui_incomplete = (
        tui_source[:insertion_offset]
        + tui_marker
        + "\n"
        + tui_source[insertion_offset:]
    )
    require_members(
        completion_labels(args.server, tui_path, tui_incomplete, tui_marker),
        {"width", "height", "cells", "draw", "fill", "clear"},
        "app.screen.buffer().",
    )

    mixed_marker = "foo.bar.xyz[(0)].get(/* nested ) ] */)."
    mixed_source = f"""module feng.examples;

type Leaf {{
  let leafMarker: string;
  func get(): Leaf {{
    return self;
  }}
}}

type Holder {{
  let xyz: Leaf[];
}}

type Root {{
  let bar: Holder;
}}

func main() {{
  let leaf = Leaf {{ leafMarker: \"ok\" }};
  let holder = Holder {{ xyz: [leaf] }};
  let foo = Root {{ bar: holder }};
  {mixed_marker}
}}
"""
    require_members(
        completion_labels(
            args.server,
            pathlib.Path("examples/hello_world/src/main.ff"),
            mixed_source,
            mixed_marker,
        ),
        {"leafMarker", "get"},
        "foo.bar.xyz[0].get().",
    )
    uncalled_marker = "foo.bar.xyz[0].get."
    uncalled_source = mixed_source.replace(mixed_marker, uncalled_marker)
    uncalled_labels = completion_labels(
        args.server,
        pathlib.Path("examples/hello_world/src/main.ff"),
        uncalled_source,
        uncalled_marker,
        wait_seconds=0.5,
    )
    if uncalled_labels:
        raise RuntimeError(
            "an uncalled method was treated as its return value: "
            f"actual={sorted(uncalled_labels)}"
        )
    print("lsp mixed receiver completion test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
