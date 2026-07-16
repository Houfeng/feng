#!/usr/bin/env python3
"""Run Feng LSP latency checks at 10K, 100K, and 1M source lines."""

from __future__ import annotations

import argparse
import json
import pathlib
import resource
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Any, BinaryIO


@dataclass
class ScenarioResult:
    """Latency samples collected for one named protocol scenario."""

    name: str
    samples: list[float]


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
    assert process.stdin is not None
    assert process.stdout is not None
    started = time.perf_counter()
    send_message(process.stdin, message)
    while True:
        response = read_message(process.stdout)
        if response.get("id") == message["id"]:
            return response, (time.perf_counter() - started) * 1000.0


def percentile(samples: list[float], percentage: float) -> float:
    """Return the nearest-rank percentile for non-empty samples."""
    ordered = sorted(samples)
    index = max(0, min(len(ordered) - 1, int(len(ordered) * percentage) - 1))
    return ordered[index]


def position_for_offset(text: str, offset: int) -> dict[str, int]:
    """Convert a Python text offset to an LSP UTF-16 position."""
    before = text[:offset]
    line_text = before.rsplit("\n", 1)[-1]
    return {
        "line": before.count("\n"),
        "character": len(line_text.encode("utf-16-le")) // 2,
    }


def interactive_source() -> str:
    """Build the fixed-size active document used at every project scale."""
    header = "module test.lsp.performance_matrix;\n" + ("// active padding\n" * 200)
    tail = (
        "func main(args: string[]) {\n"
        "    let text: string = \"feng\";\n"
        "    let value: string = text;\n"
        "}\n"
    )
    return header + tail


def prepare_project(line_count: int) -> tuple[pathlib.Path, pathlib.Path, str]:
    """Create a generated multi-file project with the requested total lines."""
    root = pathlib.Path(f"temp/lsp_matrix_{line_count}").resolve()
    if root.exists():
        shutil.rmtree(root)
    source_root = root / "src"
    source_root.mkdir(parents=True)
    source = interactive_source()
    source_path = source_root / "main.ff"
    source_path.write_text(source, encoding="utf-8")
    manifest = (
        "[package]\n"
        f'name: "lsp_matrix_{line_count}"\n'
        'version: "0.1.0"\n'
        'target: "bin"\n'
        'src: "src/"\n'
        'out: "build/"\n'
        "\n"
        "[dependencies]\n"
    )
    (root / "feng.fm").write_text(manifest, encoding="utf-8")
    remaining = max(0, line_count - source.count("\n"))
    file_index = 0
    while remaining > 0:
        lines = min(10_000, remaining)
        padding = f"module test.lsp.padding.p{file_index};\n" + ("// padding\n" * max(0, lines - 1))
        (source_root / f"padding_{file_index}.ff").write_text(padding, encoding="utf-8")
        remaining -= lines
        file_index += 1
    return root, source_path, source


def completion_request(
    request_id: int,
    uri: str,
    position: dict[str, int],
) -> dict[str, Any]:
    """Build a type completion request."""
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": "textDocument/completion",
        "params": {"textDocument": {"uri": uri}, "position": position},
    }


def hover_request(
    request_id: int,
    uri: str,
    position: dict[str, int],
) -> dict[str, Any]:
    """Build a string-literal Hover request."""
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": "textDocument/hover",
        "params": {"textDocument": {"uri": uri}, "position": position},
    }


def run_size(server: str, line_count: int, samples: int) -> tuple[list[ScenarioResult], int, float]:
    """Run all required scenarios for one generated project size."""
    project_root, source_path, source = prepare_project(line_count)
    type_start = source.index("string =", source.index("let value"))
    type_end = type_start + len("string")
    literal_start = source.index('"feng"') + 1
    uri = source_path.as_uri()
    hover_position = position_for_offset(source, literal_start + 1)
    type_start_position = position_for_offset(source, type_start)
    type_end_position = position_for_offset(source, type_end)
    completion_position = dict(type_start_position)
    completion_position["character"] += 3
    process = subprocess.Popen(
        [server, "lsp", "--stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdin is not None
    cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN)
    results: list[ScenarioResult] = []
    version = 1
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
                        "version": version,
                        "text": source,
                    }
                },
            },
        )

        response, elapsed = request(process, hover_request(10, uri, hover_position))
        if response.get("result") is None:
            raise RuntimeError(f"{line_count}: cold literal Hover returned null")
        results.append(ScenarioResult("cold_hover", [elapsed]))

        version += 1
        send_message(
            process.stdin,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": version},
                    "contentChanges": [
                        {
                            "range": {"start": type_start_position, "end": type_end_position},
                            "text": "str",
                        }
                    ],
                },
            },
        )
        response, elapsed = request(process, completion_request(11, uri, completion_position))
        labels = [item.get("label") for item in response.get("result", [])]
        if "string" not in labels:
            raise RuntimeError(f"{line_count}: cold Completion is missing string")
        results.append(ScenarioResult("edited_completion", [elapsed]))
        time.sleep(0.1)

        hot_hover: list[float] = []
        hot_completion: list[float] = []
        for sample in range(samples):
            response, elapsed = request(process, hover_request(1000 + sample, uri, hover_position))
            if response.get("result") is None:
                raise RuntimeError(f"{line_count}: hot Hover returned null")
            hot_hover.append(elapsed)
            response, elapsed = request(
                process,
                completion_request(2000 + sample, uri, completion_position),
            )
            if "string" not in [item.get("label") for item in response.get("result", [])]:
                raise RuntimeError(f"{line_count}: hot Completion is missing string")
            hot_completion.append(elapsed)
        results.append(ScenarioResult("hot_hover", hot_hover))
        results.append(ScenarioResult("hot_completion", hot_completion))

        current_prefix = "str"
        for prefix in ["s", "st", "str"] * 8:
            version += 1
            end = dict(type_start_position)
            end["character"] += len(current_prefix)
            send_message(
                process.stdin,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri, "version": version},
                        "contentChanges": [
                            {
                                "range": {"start": type_start_position, "end": end},
                                "text": prefix,
                            }
                        ],
                    },
                },
            )
            current_prefix = prefix
        response, elapsed = request(process, completion_request(3000, uri, completion_position))
        if "string" not in [item.get("label") for item in response.get("result", [])]:
            raise RuntimeError(f"{line_count}: rapid-input Completion is missing string")
        results.append(ScenarioResult("rapid_input", [elapsed]))

        version += 1
        prefix_end = dict(type_start_position)
        prefix_end["character"] += len(current_prefix)
        send_message(
            process.stdin,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": version},
                    "contentChanges": [
                        {"range": {"start": type_start_position, "end": prefix_end}, "text": "string"}
                    ],
                },
            },
        )

        end_position = position_for_offset(source, len(source))
        for name, suffix, request_id in [
            ("syntax_error", "func broken(\n", 4000),
            ("semantic_error", "func broken(): Unknown { return missing; }\n", 4001),
        ]:
            version += 1
            send_message(
                process.stdin,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri, "version": version},
                        "contentChanges": [
                            {"range": {"start": end_position, "end": end_position}, "text": suffix}
                        ],
                    },
                },
            )
            response, elapsed = request(process, hover_request(request_id, uri, hover_position))
            if response.get("result") is None:
                raise RuntimeError(f"{line_count}: {name} Hover returned null")
            results.append(ScenarioResult(name, [elapsed]))
            version += 1
            suffix_end = dict(end_position)
            suffix_end["line"] += suffix.count("\n")
            suffix_end["character"] = 0
            send_message(
                process.stdin,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri, "version": version},
                        "contentChanges": [
                            {"range": {"start": end_position, "end": suffix_end}, "text": ""}
                        ],
                    },
                },
            )

        request(process, {"jsonrpc": "2.0", "id": 9999, "method": "shutdown", "params": None})
        send_message(process.stdin, {"jsonrpc": "2.0", "method": "exit"})
        process.stdin.close()
        process.wait(timeout=120)
        cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
        rss_kib = int(cpu_after.ru_maxrss / 1024) if sys.platform == "darwin" else int(cpu_after.ru_maxrss)
        cpu_seconds = (cpu_after.ru_utime + cpu_after.ru_stime) - (
            cpu_before.ru_utime + cpu_before.ru_stime
        )
        return results, rss_kib, cpu_seconds
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        if project_root.exists():
            shutil.rmtree(project_root)


def main() -> int:
    """Run and enforce the full line-count and dirty-input matrix."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="build/bin/feng")
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--sizes", default="10000,100000,1000000")
    parser.add_argument("--no-enforce", action="store_true")
    args = parser.parse_args()
    sizes = [int(value) for value in args.sizes.split(",") if value]
    all_interactive: list[float] = []
    failures: list[str] = []

    for line_count in sizes:
        results, rss_kib, cpu_seconds = run_size(args.server, line_count, args.samples)
        print(f"lines={line_count} rss_kib={rss_kib} background_cpu_s={cpu_seconds:.3f}")
        for result in results:
            p50 = statistics.median(result.samples)
            p95 = percentile(result.samples, 0.95)
            p99 = percentile(result.samples, 0.99)
            maximum = max(result.samples)
            all_interactive.extend(result.samples)
            print(
                f"  {result.name}_ms p50={p50:.3f} p95={p95:.3f} "
                f"p99={p99:.3f} max={maximum:.3f}"
            )
            if result.name.endswith("hover") and p95 > 5.0:
                failures.append(f"{line_count} {result.name} P95 {p95:.3f}ms > 5ms")
            if "completion" in result.name and p95 > 20.0:
                failures.append(f"{line_count} {result.name} P95 {p95:.3f}ms > 20ms")

    interactive_p99 = percentile(all_interactive, 0.99)
    print(f"matrix_interactive_ms p99={interactive_p99:.3f} max={max(all_interactive):.3f}")
    if interactive_p99 > 50.0:
        failures.append(f"matrix interactive P99 {interactive_p99:.3f}ms > 50ms")
    if failures and not args.no_enforce:
        raise RuntimeError("; ".join(failures))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
