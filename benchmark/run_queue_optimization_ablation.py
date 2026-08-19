#!/usr/bin/env python3
"""Build and run the queue hot-path ablation benchmark."""

from __future__ import annotations

import argparse
import json
import os
import platform
import random
import shutil
import statistics
import subprocess
import tarfile
import tempfile
from pathlib import Path


VARIANTS = (
    ("original", "Original Hakle", 0),
    ("consumer_cache", "Optimization 1 · consumer cache", 1),
    ("producer_path", "Optimization 2 · direct token dispatch + word flags", 2),
    ("all_optimizations", "All optimizations", 3),
    ("moodycamel", "moodycamel", 4),
)

SCENARIOS = (
    (1, 1, 100_000),
    (2, 2, 100_000),
    (4, 4, 100_000),
    (8, 8, 50_000),
    (12, 12, 50_000),
    (16, 16, 50_000),
    (20, 20, 50_000),
    (24, 24, 50_000),
)

PATHS = ("implicit", "implicit_bulk", "token", "bulk")


def run(command: list[str], *, cwd: Path, capture: bool = False) -> str:
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    return completed.stdout.strip() if capture else ""


def compiler_name(compiler: str, root: Path) -> str:
    return run([compiler, "--version"], cwd=root, capture=True).splitlines()[0]


def cpu_name(root: Path) -> str:
    if platform.system() == "Darwin":
        try:
            return run(["sysctl", "-n", "machdep.cpu.brand_string"], cwd=root, capture=True)
        except subprocess.CalledProcessError:
            pass
    return platform.processor() or platform.machine()


def make_original_tree(root: Path, destination: Path) -> None:
    archive = destination.parent / "original.tar"
    run(
        ["git", "archive", "--format=tar", f"--output={archive}", "HEAD", "ConcurrentQueue", "common"],
        cwd=root,
    )
    destination.mkdir(parents=True)
    with tarfile.open(archive) as stream:
        stream.extractall(destination, filter="data")


def make_consumer_cache_tree(root: Path, destination: Path) -> None:
    shutil.copytree(root / "ConcurrentQueue", destination / "ConcurrentQueue")
    shutil.copytree(root / "common", destination / "common")
    header = destination / "ConcurrentQueue" / "ConcurrentQueue.h"
    text = header.read_text(encoding="utf-8")
    replacements = {
        "Token.ProducerNode->GetExplicitProducer()->Dequeue( Element )": "Token.ProducerNode->ProducerDequeue( Element )",
        "Token.ProducerNode->GetExplicitProducer()->DequeueBulk( ItemFirst, MaxCount )": "Token.ProducerNode->ProducerDequeueBulk( ItemFirst, MaxCount )",
        "Token.ProducerNode->GetExplicitProducer()->template Enqueue<Alloc>( std::forward<Args>( args )... )": "Token.ProducerNode->template ProducerEnqueue<Alloc>( std::forward<Args>( args )... )",
        "Token.ProducerNode->GetExplicitProducer()->template EnqueueBulk<Alloc>( ItermFirst, Count )": "Token.ProducerNode->template ProducerEnqueueBulk<Alloc>( ItermFirst, Count )",
    }
    for current, legacy in replacements.items():
        if text.count(current) != 1:
            raise RuntimeError(f"expected one direct-dispatch expression: {current}")
        text = text.replace(current, legacy)
    header.write_text(text, encoding="utf-8")


def build_variants(
    root: Path,
    temporary: Path,
    compiler: str,
    variants: tuple[tuple[str, str, int], ...],
) -> dict[str, Path]:
    original_root = temporary / "original"
    consumer_cache_root = temporary / "consumer-cache"
    make_original_tree(root, original_root)
    make_consumer_cache_tree(root, consumer_cache_root)

    include_roots = {
        "original": original_root,
        "consumer_cache": consumer_cache_root,
        "producer_path": root,
        "all_optimizations": root,
        "moodycamel": root,
    }
    source = root / "benchmark" / "queue_optimization_ablation.cc"
    binary_directory = temporary / "bin"
    binary_directory.mkdir()
    binaries: dict[str, Path] = {}
    for name, _, variant in variants:
        binary = binary_directory / name
        command = [
            compiler,
            "-std=c++20",
            "-O3",
            "-DNDEBUG",
            "-pthread",
            f"-DHAKLE_ABLATION_VARIANT={variant}",
            f"-I{include_roots[name]}",
            f"-I{root / 'benchmark' / 'third_party'}",
            str(source),
            "-o",
            str(binary),
        ]
        run(command, cwd=root)
        binaries[name] = binary
    return binaries


def collect(
    root: Path,
    binaries: dict[str, Path],
    variants: tuple[tuple[str, str, int], ...],
    paths: tuple[str, ...],
    repetitions: int,
    minimum_seconds: float,
) -> list[dict[str, object]]:
    samples: dict[tuple[str, str, int, int], list[float]] = {
        (name, path, producers, consumers): []
        for name, _, _ in variants
        for path in paths
        for producers, consumers, _ in SCENARIOS
    }
    generator = random.Random(0x48414B4C45)

    for path in paths:
        for producers, consumers, items in SCENARIOS:
            for _ in range(repetitions):
                order = [name for name, _, _ in variants]
                generator.shuffle(order)
                for name in order:
                    output = run(
                        [
                            str(binaries[name]),
                            path,
                            str(producers),
                            str(consumers),
                            str(items),
                            str(minimum_seconds),
                        ],
                        cwd=root,
                        capture=True,
                    )
                    row = json.loads(
                        next(
                            line
                            for line in reversed(output.splitlines())
                            if line.startswith("{")
                        )
                    )
                    samples[(name, path, producers, consumers)].append(
                        float(row["million_items_per_second"])
                    )

    labels = {name: label for name, label, _ in variants}
    results: list[dict[str, object]] = []
    for name, _, _ in variants:
        for path in paths:
            for producers, consumers, items in SCENARIOS:
                values = samples[(name, path, producers, consumers)]
                results.append(
                    {
                        "name": name,
                        "label": labels[name],
                        "path": path,
                        "producers": producers,
                        "consumers": consumers,
                        "items_per_producer": items,
                        "samples_million_items_per_second": values,
                        "median_million_items_per_second": statistics.median(values),
                    }
                )
    return results


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--minimum-seconds", type=float, default=0.25)
    parser.add_argument("--compiler", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--cpu-label", help="override the CPU label stored in the result")
    parser.add_argument(
        "--variants",
        nargs="+",
        choices=[name for name, _, _ in VARIANTS],
        default=[name for name, _, _ in VARIANTS],
        help="queue variants to include",
    )
    parser.add_argument(
        "--paths",
        nargs="+",
        choices=PATHS,
        default=list(PATHS),
        help="benchmark paths to include",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("benchmark/results/local-queue-optimization-ablation.json"),
    )
    arguments = parser.parse_args()
    if arguments.repetitions < 3:
        raise SystemExit("at least three repetitions are required")
    if arguments.minimum_seconds <= 0:
        raise SystemExit("--minimum-seconds must be positive")

    root = Path(__file__).resolve().parents[1]
    selected_names = set(arguments.variants)
    variants = tuple(entry for entry in VARIANTS if entry[0] in selected_names)
    paths = tuple(arguments.paths)
    with tempfile.TemporaryDirectory(prefix="hakle-queue-ablation-") as directory:
        binaries = build_variants(root, Path(directory), arguments.compiler, variants)
        results = collect(
            root,
            binaries,
            variants,
            paths,
            arguments.repetitions,
            arguments.minimum_seconds,
        )

    payload = {
        "context": {
            "host": platform.node(),
            "platform": platform.platform(),
            "cpu": arguments.cpu_label or cpu_name(root),
            "logical_cpus": os.cpu_count(),
            "compiler": compiler_name(arguments.compiler, root),
            "build": "C++20 -O3 -DNDEBUG",
            "repetitions": arguments.repetitions,
            "minimum_timed_seconds_per_sample": arguments.minimum_seconds,
            "aggregation": "median; one untimed warm-up per process; deterministic randomized variant order",
            "scenario": "Selected scalar and/or 64-item bulk MPMC workloads; equal preallocated block count",
        },
        "definitions": {
            "original": "HEAD queue before the uncommitted hot-path changes",
            "consumer_cache": "thread-local token-less consumer cache only; legacy ProducerToken dispatch and byte flags",
            "producer_path": "direct ProducerToken dispatch plus WordFlagsCheckPolicy; consumer cache disabled",
            "all_optimizations": "consumer cache, direct ProducerToken dispatch, and WordFlagsCheckPolicy",
            "moodycamel": "vendored moodycamel queue using matching implicit, token, and token-bulk paths",
        },
        "results": results,
    }
    output = arguments.output if arguments.output.is_absolute() else root / arguments.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
