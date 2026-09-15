#!/usr/bin/env python3
"""Verify match binding hover, definition, completion, and request stability."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import os
import pathlib
import queue
import subprocess
import tempfile
import threading
import time
from typing import Any, Iterator

from test_lsp_completion_recovery import (
    position_for_offset,
    read_message,
    send_message,
)


SOURCE = """module test.lsp.match_bindings;
type Box<T> {
  let value: T;
  func clear() {}
}
type Empty {}
spec Choice: Box<string> | string | i32 | Empty;
spec Outer: Choice | bool;

func statement(choice: Choice, output: i32) {
  match choice {
    output: Box<string> {
      output.clear();
      match choice {
        var output: string { let nested = output; }
        else { let outer = output; }
      }
      {
        let output: i32 = 3;
        let shadow = output;
      }
      let restored = output;
    }
    output: string { let sibling = output; }
    else { let fallback = output; }
  }
  let after = output;
}

func expression(choice: Choice): i32 {
  return match choice {
    var item: Box<string> { item.clear(); 1; }
    item: string { let text = item; 2; }
    else { 0; }
  };
}

func patterns(choice: Choice, outer: Outer) {
  match choice {
    let subset: Box<string>, string { let selected = subset; }
    else {}
  }
  match outer {
    leaf: Choice -> Box<string> { leaf.clear(); }
    else {}
  }
  if choice match var selected: Box<string> { selected.clear(); }
}

func makeBox(): Box<string> { return Box<string> { value: "ok" }; }
func inferredProbe() { return makeBox(); }
"""


class Client:
    """A framed LSP client with bounded response waits and burst support."""

    def __init__(self, process: subprocess.Popen[bytes], uri: str, source: str):
        """Start a reader so stalled or crashed servers fail with a timeout."""
        self.process = process
        self.uri = uri
        self.source = source
        self.next_id = 0
        self.messages: queue.Queue[Any] = queue.Queue()
        self.pending: dict[int, dict[str, Any]] = {}
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self) -> None:
        """Forward responses or transport failures from the server pipe."""
        assert self.process.stdout is not None
        try:
            while True:
                self.messages.put(read_message(self.process.stdout))
        except Exception as error:
            self.messages.put(error)

    def send(self, method: str, params: Any, request: bool = True) -> int:
        """Send a request or notification without waiting for a response."""
        assert self.process.stdin is not None
        message: dict[str, Any] = {"jsonrpc": "2.0", "method": method, "params": params}
        if request:
            self.next_id += 1
            message["id"] = self.next_id
        send_message(self.process.stdin, message)
        return self.next_id

    def receive(self, request_id: int) -> Any:
        """Receive one response while preserving out-of-order burst replies."""
        deadline = time.monotonic() + 30
        while request_id not in self.pending:
            message = self.messages.get(timeout=max(0.001, deadline - time.monotonic()))
            if isinstance(message, Exception):
                raise message
            if "id" in message:
                self.pending[message["id"]] = message
        response = self.pending.pop(request_id)
        if "error" in response:
            raise AssertionError(response)
        return response.get("result")

    def request(self, method: str, params: Any) -> Any:
        """Send and wait for one request."""
        return self.receive(self.send(method, params))

    def position(self, marker: str, delta: int = 0) -> dict[str, int]:
        """Locate an unambiguous source marker using LSP UTF-16 columns."""
        assert self.source.count(marker) == 1, marker
        return position_for_offset(self.source, self.source.index(marker) + delta)

    def query(self, method: str, marker: str, delta: int = 0) -> Any:
        """Request one position-based language feature."""
        return self.request(
            "textDocument/" + method,
            {"textDocument": {"uri": self.uri}, "position": self.position(marker, delta)},
        )

    def check_binding(self, marker: str, name: str, signature: str, declaration: str) -> None:
        """Require the correct hover and exact identifier definition range."""
        delta = marker.rindex(name)
        hover = self.query("hover", marker, delta)
        assert hover is not None and signature in hover["contents"]["value"], (marker, hover)
        location = self.query("definition", marker, delta)
        start = self.position(declaration, declaration.index(name))
        end = dict(start, character=start["character"] + len(name))
        assert location == {"uri": self.uri, "range": {"start": start, "end": end}}, (
            marker, location, start
        )

    def check_members(self, marker: str, required: set[str]) -> None:
        """Require members of the narrowed receiver at its dot."""
        items = self.query("completion", marker, marker.index(".") + 1)
        labels = {item["label"] for item in items}
        assert required.issubset(labels), (marker, labels)
        assert not {"Choice", "Empty", "break", "match"}.intersection(labels), labels


@contextmanager
def session(server: str, path: pathlib.Path, source: str, directory: pathlib.Path,
            markdown: bool = False) -> Iterator[Client]:
    """Run one server, checking clean shutdown and sanitizer diagnostics."""
    environment = dict(os.environ, FENG_TEMP_DIR=str(directory.resolve()))
    with (directory / "stderr.log").open("w+") as errors:
        process = subprocess.Popen(
            [server, "lsp", "--stdio"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=errors, env=environment,
        )
        client = Client(process, path.resolve().as_uri(), source)
        try:
            capabilities = {"textDocument": {"hover": {"contentFormat": ["markdown"]}}} if markdown else {}
            client.request("initialize", {"capabilities": capabilities})
            client.send("initialized", {}, request=False)
            client.send("textDocument/didOpen", {"textDocument": {
                "uri": client.uri, "languageId": "feng", "version": 1, "text": source,
            }}, request=False)
            yield client
            client.request("shutdown", None)
            client.send("exit", None, request=False)
            assert process.stdin is not None
            process.stdin.close()
            assert process.wait(timeout=30) == 0
            errors.seek(0)
            stderr = errors.read()
            assert "runtime error:" not in stderr and "Sanitizer" not in stderr, stderr
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            client.reader.join(timeout=2)


def check_scopes(client: Client) -> None:
    """Cover declarations, nested/sibling scopes, expressions, and patterns."""
    cases = [
        ("output: Box<string>", "output", "let output: Box<string>", "output: Box<string>"),
        ("output.clear()", "output", "let output: Box<string>", "output: Box<string>"),
        ("var output: string", "output", "var output: string", "var output: string"),
        ("nested = output", "output", "var output: string", "var output: string"),
        ("outer = output", "output", "let output: Box<string>", "output: Box<string>"),
        ("shadow = output", "output", "let output: i32", "let output: i32 = 3"),
        ("restored = output", "output", "let output: Box<string>", "output: Box<string>"),
        ("sibling = output", "output", "let output: string", "output: string { let sibling"),
        ("fallback = output", "output", "param let output: i32", "output: i32)"),
        ("after = output", "output", "param let output: i32", "output: i32)"),
        ("var item: Box<string>", "item", "var item: Box<string>", "var item: Box<string>"),
        ("item.clear()", "item", "var item: Box<string>", "var item: Box<string>"),
        ("text = item", "item", "let item: string", "item: string"),
        ("let subset: Box<string>, string", "subset", "let subset: Box<string> | string", "let subset: Box<string>, string"),
        ("selected = subset", "subset", "let subset: Box<string> | string", "let subset: Box<string>, string"),
        ("leaf: Choice -> Box<string>", "leaf", "let leaf: Box<string>", "leaf: Choice -> Box<string>"),
        ("leaf.clear()", "leaf", "let leaf: Box<string>", "leaf: Choice -> Box<string>"),
        ("var selected: Box<string>", "selected", "var selected: Box<string>", "var selected: Box<string>"),
        ("selected.clear()", "selected", "var selected: Box<string>", "var selected: Box<string>"),
    ]
    for case in cases:
        client.check_binding(*case)
    for marker in ("output.clear()", "item.clear()", "leaf.clear()", "selected.clear()"):
        client.check_members(marker, {"value", "clear"})


def check_real_file(client: Client) -> None:
    """Stress the reported nested loops with repeated and queued requests."""
    declaration = "output: List<string> {"
    markers = [declaration, "i < output.size(); i += 1 {\n              println",
               "p.add(output.get(i))", "println(output.get(i))", "output.clear()"]
    for marker in markers:
        client.check_binding(marker, "output", "let output: List<string>", declaration)
    client.check_members("output.clear()", {"size", "get", "clear", "add"})
    position = client.position("println(output.get(i))", len("println("))
    requests = []
    for _ in range(32):
        for method in ("hover", "definition"):
            request_id = client.send("textDocument/" + method, {
                "textDocument": {"uri": client.uri}, "position": position,
            })
            requests.append((request_id, method))
    for request_id, method in requests:
        result = client.receive(request_id)
        if method == "hover":
            assert result is not None and "let output: List<string>" in result["contents"]["value"], result
        else:
            assert result is not None and result["range"]["start"] == client.position(declaration), result


def main() -> int:
    """Exercise real and synthetic files, including edits that shift positions."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="build/bin/feng")
    args = parser.parse_args()
    pathlib.Path("temp").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lsp_match_bindings_", dir="temp") as temporary:
        directory = pathlib.Path(temporary)
        path = directory / "main.ff"
        path.write_text(SOURCE, encoding="utf-8")
        with session(args.server, path, SOURCE, directory) as client:
            check_scopes(client)
            deadline = time.monotonic() + 15
            while True:
                hover = client.query("hover", "func inferredProbe()", len("func "))
                if hover and "inferredProbe(): Box<string>" in hover["contents"]["value"]:
                    break
                assert time.monotonic() < deadline, ("semantic analysis did not become ready", hover)
                time.sleep(0.05)
            check_scopes(client)
            client.source = "// shifted overlay\n" + SOURCE + "\nfunc unfinished() { missing(); }\n"
            client.send("textDocument/didChange", {
                "textDocument": {"uri": client.uri, "version": 2},
                "contentChanges": [{"text": client.source}],
            }, request=False)
            check_scopes(client)
        path = pathlib.Path("std/std/src/test/TestContext.ff")
        with session(args.server, path, path.read_text(encoding="utf-8"), directory, markdown=True) as client:
            check_real_file(client)
    print("lsp match binding tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
