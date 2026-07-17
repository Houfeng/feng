#!/usr/bin/env python3
"""Verify incomplete member access never falls back to global completion."""

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
    """Type `user.` incrementally and validate the resulting candidate scope."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="build/bin/feng")
    args = parser.parse_args()

    valid_source = (
        "module test.lsp.member_completion_scope;\n"
        "\n"
        "import std;\n"
        "\n"
        "type User {\n"
        "    var name: string;\n"
        "    var age: int;\n"
        "}\n"
        "\n"
        "func main(args: string[]) {\n"
        "    let user = User { name: \"alice\", age: 20 };\n"
        "    \n"
        "    println(\"ready\");\n"
        "}\n"
    )
    insertion_offset = valid_source.index("    \n    println") + 4
    current_source = valid_source
    uri = pathlib.Path("temp/lsp_member_completion_scope.ff").resolve().as_uri()
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
                        "text": valid_source,
                    }
                },
            },
        )
        time.sleep(0.1)

        version = 1
        request_id = 1
        cursor_offset = insertion_offset
        for character in "user.":
            version += 1
            edit_position = position_for_offset(current_source, cursor_offset)
            send_message(
                process.stdin,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri, "version": version},
                        "contentChanges": [
                            {
                                "range": {
                                    "start": edit_position,
                                    "end": edit_position,
                                },
                                "text": character,
                            }
                        ],
                    },
                },
            )
            current_source = (
                current_source[:cursor_offset]
                + character
                + current_source[cursor_offset:]
            )
            cursor_offset += 1
            if character == ".":
                request_id += 1
                labels = completion_request(
                    process,
                    request_id,
                    uri,
                    position_for_offset(current_source, cursor_offset),
                )

        required = {"name", "age"}
        unrelated = {"Action", "args", "assert", "break"}
        if not required.issubset(labels):
            raise RuntimeError(
                "member completion is missing User members: "
                f"required={sorted(required)}, actual={sorted(labels)}"
            )
        leaked = labels.intersection(unrelated)
        if leaked:
            raise RuntimeError(
                "member completion leaked global candidates: "
                f"leaked={sorted(leaked)}, actual={sorted(labels)}"
            )

        request(
            process,
            {"jsonrpc": "2.0", "id": 9, "method": "shutdown", "params": None},
        )
        send_message(process.stdin, {"jsonrpc": "2.0", "method": "exit"})
        process.stdin.close()
        process.wait(timeout=30)
        print("lsp member completion scope test passed")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
