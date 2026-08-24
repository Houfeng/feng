#!/usr/bin/env python3
"""Measure inferred callable Hover and call-result Completion latency."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import statistics
import subprocess
import time
from dataclasses import dataclass
from typing import Any

from run_lsp_performance import percentile, position_for_offset, request, send_message


@dataclass
class ScenarioResult:
    """Latency samples collected for one protocol scenario."""

    name: str
    samples: list[float]


def hover_request(
    process: subprocess.Popen[bytes],
    request_id: int,
    uri: str,
    position: dict[str, int],
) -> tuple[dict[str, Any], float]:
    """Send one Hover request."""
    return request(
        process,
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "textDocument/hover",
            "params": {
                "textDocument": {"uri": uri},
                "position": position,
            },
        },
    )


def completion_request(
    process: subprocess.Popen[bytes],
    request_id: int,
    uri: str,
    position: dict[str, int],
) -> tuple[dict[str, Any], float]:
    """Send one Completion request."""
    return request(
        process,
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": position,
            },
        },
    )


def hover_text(response: dict[str, Any]) -> str:
    """Return the textual contents of one Hover response."""
    result = response.get("result")
    if not isinstance(result, dict):
        return ""
    contents = result.get("contents")
    if isinstance(contents, dict):
        value = contents.get("value")
        return value if isinstance(value, str) else ""
    return contents if isinstance(contents, str) else ""


def completion_labels(response: dict[str, Any]) -> list[str]:
    """Return string labels from one Completion response."""
    result = response.get("result")
    if not isinstance(result, list):
        return []
    return [
        label
        for item in result
        if isinstance(item, dict)
        for label in [item.get("label")]
        if isinstance(label, str)
    ]


def wait_for_hover_text(
    process: subprocess.Popen[bytes],
    request_id: int,
    uri: str,
    position: dict[str, int],
    expected: str,
) -> tuple[int, list[float]]:
    """Poll Hover responses until one expected Semantic result is visible."""
    deadline = time.monotonic() + 10.0
    samples: list[float] = []
    latest = ""
    while time.monotonic() < deadline:
        response, elapsed = hover_request(process, request_id, uri, position)
        request_id += 1
        samples.append(elapsed)
        latest = hover_text(response)
        if expected in latest:
            return request_id, samples
    raise RuntimeError(f"Semantic Hover readiness was not observed: {latest!r}")


def wait_for_completion_labels(
    process: subprocess.Popen[bytes],
    request_id: int,
    uri: str,
    position: dict[str, int],
    expected: set[str],
) -> tuple[int, list[float]]:
    """Poll Completion until every expected call-result member is visible."""
    deadline = time.monotonic() + 10.0
    samples: list[float] = []
    latest: list[str] = []
    while time.monotonic() < deadline:
        response, elapsed = completion_request(process, request_id, uri, position)
        request_id += 1
        samples.append(elapsed)
        latest = completion_labels(response)
        if expected.issubset(latest):
            return request_id, samples
    raise RuntimeError(f"Semantic Completion readiness was not observed: {latest!r}")


def collect_hover(
    process: subprocess.Popen[bytes],
    request_id: int,
    uri: str,
    position: dict[str, int],
    expected: str,
    sample_count: int,
) -> tuple[int, list[float]]:
    """Collect validated Hover latency samples."""
    samples: list[float] = []
    for _ in range(sample_count):
        response, elapsed = hover_request(process, request_id, uri, position)
        request_id += 1
        actual = hover_text(response)
        if expected not in actual:
            raise RuntimeError(f"Hover is missing {expected!r}: {actual!r}")
        samples.append(elapsed)
    return request_id, samples


def collect_completion(
    process: subprocess.Popen[bytes],
    request_id: int,
    uri: str,
    position: dict[str, int],
    expected: set[str],
    sample_count: int,
) -> tuple[int, list[float]]:
    """Collect validated call-result Completion latency samples."""
    samples: list[float] = []
    for _ in range(sample_count):
        response, elapsed = completion_request(process, request_id, uri, position)
        request_id += 1
        labels = completion_labels(response)
        if not expected.issubset(labels):
            raise RuntimeError(
                f"Completion is missing {sorted(expected)!r}: {labels!r}"
            )
        samples.append(elapsed)
    return request_id, samples


def send_full_change(
    process: subprocess.Popen[bytes],
    uri: str,
    version: int,
    source: str,
) -> None:
    """Publish one full-document edit without adding a request-path wait."""
    assert process.stdin is not None
    send_message(
        process.stdin,
        {
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"text": source}],
            },
        },
    )


def start_lsp(server: pathlib.Path) -> subprocess.Popen[bytes]:
    """Start the real stdio language server."""
    return subprocess.Popen(
        [str(server), "lsp", "--stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def initialize_and_open(
    process: subprocess.Popen[bytes],
    uri: str,
    source: str,
) -> None:
    """Initialize one LSP session and open its measured document."""
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
    assert process.stdin is not None
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


def stop_lsp(process: subprocess.Popen[bytes], request_id: int) -> None:
    """Shut down one measured LSP session cleanly."""
    request(
        process,
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "shutdown",
            "params": None,
        },
    )
    assert process.stdin is not None
    send_message(process.stdin, {"jsonrpc": "2.0", "method": "exit"})
    process.stdin.close()
    process.wait(timeout=30)


def write_main_project(root: pathlib.Path) -> tuple[pathlib.Path, str]:
    """Create the AST-backed inferred callable performance fixture."""
    source = (
        "module test.lsp.inferred_callable_performance;\n"
        "type ResultBox {\n"
        "  let value: string;\n"
        "  func label(): string { return self.value; }\n"
        "}\n"
        "type Owner {\n"
        "  func instanceSelect(flag: bool) {\n"
        "    if flag { return \"left\"; }\n"
        "    return \"right\";\n"
        "  }\n"
        "  static func staticSelect(flag: bool) {\n"
        "    if flag { return \"left\"; }\n"
        "    return \"right\";\n"
        "  }\n"
        "  func makeBox() { return ResultBox { value: \"instance\" }; }\n"
        "}\n"
        "type FitTarget {}\n"
        "fit FitTarget {\n"
        "  func fitInstanceSelect(flag: bool) {\n"
        "    if flag { return \"left\"; }\n"
        "    return \"right\";\n"
        "  }\n"
        "  static func fitStaticSelect(flag: bool) {\n"
        "    if flag { return \"left\"; }\n"
        "    return \"right\";\n"
        "  }\n"
        "}\n"
        "func topSelect(flag: bool) {\n"
        "  if flag { return \"left\"; }\n"
        "  return \"right\";\n"
        "}\n"
        "func makeBox() { return ResultBox { value: \"top\" }; }\n"
        "func use(owner: Owner, target: FitTarget) {\n"
        "  let ready = topSelect(true);\n"
        "  topSelect(false);\n"
        "  owner.instanceSelect(true);\n"
        "  Owner.staticSelect(true);\n"
        "  target.fitInstanceSelect(true);\n"
        "  FitTarget.fitStaticSelect(true);\n"
        "  let topValue = makeBox().value;\n"
        "  let instanceValue = owner.makeBox().value;\n"
        "}\n"
    )
    source_root = root / "src"
    source_root.mkdir(parents=True)
    source_path = source_root / "main.ff"
    source_path.write_text(source, encoding="utf-8")
    (root / "feng.fm").write_text(
        "[package]\n"
        'name: "lsp_inferred_callable_performance"\n'
        'version: "0.1.0"\n'
        'target: "lib"\n'
        'src: "src/"\n'
        'out: "build/"\n',
        encoding="utf-8",
    )
    return source_path, source


def run_ast_scenarios(
    server: pathlib.Path,
    root: pathlib.Path,
    sample_count: int,
) -> list[ScenarioResult]:
    """Run current-file Hover, Completion, edit, and error scenarios."""
    source_path, source = write_main_project(root)
    uri = source_path.as_uri()
    readiness_position = position_for_offset(source, source.index("ready =") + 1)
    hover_specs = [
        (
            "top_level_hover",
            position_for_offset(source, source.index("topSelect(flag: bool)") + 1),
            "func topSelect(flag: bool): string",
        ),
        (
            "instance_method_hover",
            position_for_offset(source, source.index("instanceSelect(flag: bool)") + 1),
            "func instanceSelect(flag: bool): string",
        ),
        (
            "static_method_hover",
            position_for_offset(source, source.index("staticSelect(flag: bool)") + 1),
            "func staticSelect(flag: bool): string",
        ),
        (
            "fit_instance_method_hover",
            position_for_offset(source, source.index("fitInstanceSelect(flag: bool)") + 1),
            "func fitInstanceSelect(flag: bool): string",
        ),
        (
            "fit_static_method_hover",
            position_for_offset(source, source.index("fitStaticSelect(flag: bool)") + 1),
            "func fitStaticSelect(flag: bool): string",
        ),
    ]
    top_completion_offset = source.index("makeBox().value") + len("makeBox().")
    instance_completion_offset = source.index("owner.makeBox().value") + len(
        "owner.makeBox()."
    )
    completion_specs = [
        (
            "top_level_call_result_completion",
            position_for_offset(source, top_completion_offset),
        ),
        (
            "instance_call_result_completion",
            position_for_offset(source, instance_completion_offset),
        ),
    ]
    process = start_lsp(server)
    request_id = 10
    results: list[ScenarioResult] = []
    try:
        initialize_and_open(process, uri, source)
        request_id, samples = wait_for_hover_text(
            process,
            request_id,
            uri,
            readiness_position,
            "let ready: string",
        )
        results.append(ScenarioResult("cold_hover_readiness", samples))

        for name, position, expected in hover_specs:
            request_id, samples = collect_hover(
                process,
                request_id,
                uri,
                position,
                expected,
                sample_count,
            )
            results.append(ScenarioResult(name, samples))

        for name, position in completion_specs:
            request_id, readiness_samples = wait_for_completion_labels(
                process,
                request_id,
                uri,
                position,
                {"value", "label"},
            )
            results.append(ScenarioResult(f"{name}_readiness", readiness_samples))
            request_id, samples = collect_completion(
                process,
                request_id,
                uri,
                position,
                {"value", "label"},
                sample_count,
            )
            results.append(ScenarioResult(name, samples))

        syntax_source = source + "func broken(\n"
        send_full_change(process, uri, 2, syntax_source)
        request_id, samples = collect_hover(
            process,
            request_id,
            uri,
            hover_specs[0][1],
            hover_specs[0][2],
            sample_count,
        )
        results.append(ScenarioResult("syntax_error_hover", samples))

        semantic_source = source + "func broken(value: MissingType): void {}\n"
        send_full_change(process, uri, 3, semantic_source)
        request_id, samples = collect_hover(
            process,
            request_id,
            uri,
            hover_specs[0][1],
            hover_specs[0][2],
            sample_count,
        )
        results.append(ScenarioResult("semantic_error_hover", samples))

        original_top = (
            "func topSelect(flag: bool) {\n"
            "  if flag { return \"left\"; }\n"
            "  return \"right\";\n"
            "}\n"
        )
        recovered_top = (
            "func topSelect(flag: bool) {\n"
            "  if flag { return ResultBox { value: \"left\" }; }\n"
            "  return ResultBox { value: \"right\" };\n"
            "}\n"
        )
        recovered_source = source.replace(original_top, recovered_top, 1)
        send_full_change(process, uri, 4, recovered_source)
        request_id, recovery_readiness = wait_for_hover_text(
            process,
            request_id,
            uri,
            hover_specs[0][1],
            "func topSelect(flag: bool): ResultBox",
        )
        results.append(ScenarioResult("recovery_hover_readiness", recovery_readiness))
        request_id, samples = collect_hover(
            process,
            request_id,
            uri,
            hover_specs[0][1],
            "func topSelect(flag: bool): ResultBox",
            sample_count,
        )
        results.append(ScenarioResult("recovery_hover", samples))

        recovered_top_completion_offset = recovered_source.index("makeBox().value") + len(
            "makeBox()."
        )
        recovered_completion_position = position_for_offset(
            recovered_source, recovered_top_completion_offset
        )
        request_id, recovery_completion_readiness = wait_for_completion_labels(
            process,
            request_id,
            uri,
            recovered_completion_position,
            {"value", "label"},
        )
        results.append(
            ScenarioResult(
                "recovery_completion_readiness", recovery_completion_readiness
            )
        )
        request_id, samples = collect_completion(
            process,
            request_id,
            uri,
            recovered_completion_position,
            {"value", "label"},
            sample_count,
        )
        results.append(ScenarioResult("recovery_completion", samples))

        stop_lsp(process, request_id)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    return results


def write_external_projects(
    server: pathlib.Path,
    root: pathlib.Path,
) -> tuple[pathlib.Path, str]:
    """Build an external package and create its symbol-backed consumer."""
    package_root = root / "external_package"
    package_source_root = package_root / "src"
    package_source_root.mkdir(parents=True)
    package_source = (
        "open module test.lsp.inferred_performance_external;\n"
        "open type ExternalBox { open let value: string; }\n"
        "open func makeExternalBox() {\n"
        "  return ExternalBox { value: \"external\" };\n"
        "}\n"
    )
    package_source_path = package_source_root / "main.ff"
    package_source_path.write_text(package_source, encoding="utf-8")
    (package_root / "feng.fm").write_text(
        "[package]\n"
        'name: "lsp_inferred_performance_external"\n'
        'version: "0.1.0"\n'
        'target: "lib"\n'
        'src: "src/"\n'
        'out: "build/"\n',
        encoding="utf-8",
    )
    packed = subprocess.run(
        [str(server), "pack", str(package_root)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if packed.returncode != 0:
        raise RuntimeError(
            "failed to build external performance package: "
            + packed.stderr.strip()
        )
    bundle = (
        package_root
        / "build/pkg/lsp_inferred_performance_external-0.1.0.fb"
    )
    if not bundle.is_file():
        raise RuntimeError(f"external package bundle is missing: {bundle}")
    package_source_path.unlink()

    consumer_root = root / "external_consumer"
    consumer_source_root = consumer_root / "src"
    consumer_source_root.mkdir(parents=True)
    consumer_source = (
        "module test.lsp.inferred_performance_consumer;\n"
        "import test.lsp.inferred_performance_external;\n"
        "func useExternal() {\n"
        "  let box = makeExternalBox();\n"
        "}\n"
    )
    consumer_source_path = consumer_source_root / "main.ff"
    consumer_source_path.write_text(consumer_source, encoding="utf-8")
    (consumer_root / "feng.fm").write_text(
        "[package]\n"
        'name: "lsp_inferred_performance_consumer"\n'
        'version: "0.1.0"\n'
        'target: "lib"\n'
        'src: "src/"\n'
        'out: "build/"\n'
        "[dependencies]\n"
        'lsp_inferred_performance_external: '
        '"../external_package/build/pkg/'
        'lsp_inferred_performance_external-0.1.0.fb"\n',
        encoding="utf-8",
    )
    return consumer_source_path, consumer_source


def run_external_hover_scenario(
    server: pathlib.Path,
    root: pathlib.Path,
    sample_count: int,
) -> list[ScenarioResult]:
    """Measure the already-correct external symbol-backed Hover path."""
    source_path, source = write_external_projects(server, root)
    uri = source_path.as_uri()
    position = position_for_offset(source, source.index("makeExternalBox()") + 1)
    process = start_lsp(server)
    request_id = 7000
    results: list[ScenarioResult] = []
    try:
        initialize_and_open(process, uri, source)
        request_id, readiness_samples = wait_for_hover_text(
            process,
            request_id,
            uri,
            position,
            "func makeExternalBox(): ExternalBox",
        )
        results.append(
            ScenarioResult("external_symbol_hover_readiness", readiness_samples)
        )
        request_id, samples = collect_hover(
            process,
            request_id,
            uri,
            position,
            "func makeExternalBox(): ExternalBox",
            sample_count,
        )
        results.append(ScenarioResult("external_symbol_hover", samples))
        stop_lsp(process, request_id)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    return results


def print_and_enforce(results: list[ScenarioResult], enforce: bool) -> None:
    """Print percentiles and enforce the shared 16ms maximum."""
    failures: list[str] = []
    for result in results:
        p50 = statistics.median(result.samples)
        p95 = percentile(result.samples, 0.95)
        p99 = percentile(result.samples, 0.99)
        maximum = max(result.samples)
        print(
            f"{result.name}_ms p50={p50:.3f} p95={p95:.3f} "
            f"p99={p99:.3f} max={maximum:.3f} samples={len(result.samples)}"
        )
        if maximum > 16.0:
            failures.append(f"{result.name} Max {maximum:.3f}ms > 16ms")
    if enforce and failures:
        raise RuntimeError("; ".join(failures))


def main() -> int:
    """Run all inferred callable protocol latency scenarios."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="build/bin/feng")
    parser.add_argument("--samples", type=int, default=200)
    parser.add_argument("--no-enforce", action="store_true")
    args = parser.parse_args()
    if args.samples < 200:
        parser.error("--samples must be at least 200")
    server = pathlib.Path(args.server).resolve()
    if not server.is_file():
        parser.error(f"server does not exist: {server}")
    root = pathlib.Path("temp/lsp_inferred_callable_performance").resolve()
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    try:
        results = run_ast_scenarios(server, root / "ast_project", args.samples)
        results.extend(
            run_external_hover_scenario(server, root, args.samples)
        )
        print_and_enforce(results, not args.no_enforce)
    finally:
        if root.exists():
            shutil.rmtree(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
