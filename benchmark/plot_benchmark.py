#!/usr/bin/env python3
"""Plot queue throughput from Google Benchmark JSON output."""

from __future__ import annotations

import argparse
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

try:
    import matplotlib.pyplot as plt
except ModuleNotFoundError as error:
    raise SystemExit(
        "matplotlib is required; install it with: python -m pip install matplotlib"
    ) from error


@dataclass(frozen=True)
class Series:
    benchmark: str
    label: str
    group: str
    color: str
    marker: str
    linestyle: str


SERIES = (
    Series("BM_HakleImplicit", "Hakle implicit", "single", "#0072B2", "o", "-"),
    Series("BM_HakleTokens", "Hakle token", "single", "#009E73", "s", "-"),
    Series(
        "BM_MoodycamelImplicit",
        "Moodycamel implicit",
        "single",
        "#56B4E9",
        "^",
        "--",
    ),
    Series(
        "BM_MoodycamelTokens",
        "Moodycamel token",
        "single",
        "#E69F00",
        "D",
        "--",
    ),
    Series("BM_BoostLockfree", "Boost.Lockfree", "single", "#D55E00", "v", ":"),
    Series("BM_OneTBB", "oneTBB", "single", "#CC79A7", "P", "-."),
    Series("BM_MutexQueue", "Mutex queue", "single", "#7F7F7F", "X", ":"),
    Series(
        "BM_HakleTokenBulk",
        "Hakle token bulk",
        "bulk",
        "#332288",
        "*",
        "-",
    ),
    Series(
        "BM_MoodycamelTokenBulk",
        "Moodycamel token bulk",
        "bulk",
        "#AA4499",
        "h",
        "--",
    ),
)

ARGUMENTS = re.compile(r"/producers:(\d+)/consumers:(\d+)/")
TEXT = "#24292F"
MUTED = "#57606A"
GRID = "#D0D7DE"


def parse_results(path: Path) -> dict[str, dict[tuple[int, int], float]]:
    with path.open(encoding="utf-8") as stream:
        document = json.load(stream)

    known = {series.benchmark for series in SERIES}
    results: dict[str, dict[tuple[int, int], float]] = {
        benchmark: {} for benchmark in known
    }

    for row in document.get("benchmarks", []):
        if row.get("run_type") == "aggregate":
            continue

        name = row.get("name", "")
        benchmark = name.split("/", 1)[0]
        match = ARGUMENTS.search(name)
        throughput = row.get("items_per_second")
        if benchmark not in known or match is None or throughput is None:
            continue

        configuration = (int(match.group(1)), int(match.group(2)))
        results[benchmark][configuration] = float(throughput) / 1_000_000.0

    return results


def validate_results(
    results: dict[str, dict[tuple[int, int], float]],
) -> list[tuple[int, int]]:
    configurations = sorted(
        {configuration for values in results.values() for configuration in values},
        key=lambda value: (sum(value), value[0], value[1]),
    )
    if len(configurations) < 2:
        raise SystemExit("the benchmark JSON must contain at least two P/C configurations")

    missing = [
        f"{series.label}: {producers}P/{consumers}C"
        for series in SERIES
        for producers, consumers in configurations
        if (producers, consumers) not in results[series.benchmark]
    ]
    if missing:
        raise SystemExit("missing benchmark results:\n  " + "\n  ".join(missing))

    return configurations


def endpoint_positions(
    values: Iterable[tuple[Series, float]], minimum_log_gap: float
) -> dict[str, float]:
    ordered = sorted(
        ((series, math.log10(value)) for series, value in values),
        key=lambda item: item[1],
    )
    adjusted: list[list[object]] = []
    for series, log_value in ordered:
        label_log_value = log_value
        if adjusted:
            label_log_value = max(
                label_log_value,
                float(adjusted[-1][1]) + minimum_log_gap,
            )
        adjusted.append([series, label_log_value])

    if adjusted:
        highest_value = max(log_value for _, log_value in ordered)
        shift = max(0.0, float(adjusted[-1][1]) - highest_value)
        for item in adjusted:
            item[1] = float(item[1]) - shift / 2.0

    return {
        item[0].benchmark: 10.0 ** float(item[1])
        for item in adjusted
    }

def style_axis(axis: plt.Axes, axis_index: int) -> None:
    axis.set_facecolor("none")
    axis.grid(axis="y", color=GRID, linewidth=0.8)
    axis.set_axisbelow(True)
    axis.tick_params(colors=MUTED, labelsize=10)
    axis.spines["top"].set_visible(False)
    axis.spines["right"].set_visible(False)
    axis.spines["left"].set_color(MUTED)
    axis.spines["bottom"].set_color(MUTED)

    for grid_index, gridline in enumerate(
        axis.get_xgridlines() + axis.get_ygridlines()
    ):
        gridline.set_gid(f"grid-{axis_index}-{grid_index}")
    for name, spine in axis.spines.items():
        spine.set_gid(f"axis-spine-{axis_index}-{name}")


def plot_all_series(
    axis: plt.Axes,
    configurations: list[tuple[int, int]],
    results: dict[str, dict[tuple[int, int], float]],
) -> None:
    x_values = list(range(len(configurations)))
    final_values = [
        (series, results[series.benchmark][configurations[-1]])
        for series in SERIES
    ]
    label_positions = endpoint_positions(final_values, minimum_log_gap=0.065)

    for series in SERIES:
        values = [
            results[series.benchmark][configuration]
            for configuration in configurations
        ]
        is_bulk = series.group == "bulk"
        line = axis.plot(
            x_values,
            values,
            color=series.color,
            marker=series.marker,
            linestyle=series.linestyle,
            linewidth=2.6 if is_bulk else 2.1,
            markersize=8.0 if is_bulk else 6.5,
            markeredgecolor="white",
            markeredgewidth=0.8,
            zorder=4 if is_bulk else 3,
        )[0]
        line.set_gid(f"series-{series.benchmark}")

        endpoint = values[-1]
        label_y = label_positions[series.benchmark]
        axis.plot(
            [x_values[-1] + 0.03, x_values[-1] + 0.20],
            [endpoint, label_y],
            color=series.color,
            linewidth=1.0,
            zorder=2,
        )
        axis.text(
            x_values[-1] + 0.23,
            label_y,
            f"{series.label}  {endpoint:.2f}",
            color=TEXT,
            fontsize=9.2,
            va="center",
        )

    axis.set_yscale("log")
    axis.set_ylim(1.5, 1_300)
    axis.set_yticks([2, 5, 10, 20, 50, 100, 200, 500, 1_000])
    axis.set_yticklabels(["2", "5", "10", "20", "50", "100", "200", "500", "1,000"])
    axis.set_xticks(
        x_values,
        [f"{producers}P / {consumers}C" for producers, consumers in configurations],
    )
    axis.set_xlim(-0.12, len(configurations) - 1 + 1.35)
    axis.set_ylabel("Throughput (million items/s, log scale)", color=TEXT)
    axis.set_xlabel("Producer / consumer threads", color=MUTED)

def inject_accessibility_and_dark_theme(path: Path) -> None:
    svg = path.read_text(encoding="utf-8")
    svg = svg.replace(
        "<svg ",
        '<svg role="img" aria-labelledby="chart-title chart-desc" ',
        1,
    )
    svg_tag_end = svg.index(">", svg.index("<svg"))
    additions = """
 <title id="chart-title">Queue throughput by producer and consumer configuration</title>
 <desc id="chart-desc">Each line represents one queue implementation or calling mode. The horizontal axis shows one producer and one consumer, four producers and four consumers, and sixteen producers and eight consumers. The vertical axis shows throughput in millions of items per second on a logarithmic scale so single-item and bulk operations remain visible together.</desc>
 <style>
 @media (prefers-color-scheme: dark) {
   text { fill: #f0f6fc !important; }
   g[id^="grid-"] path { stroke: #30363d !important; }
   g[id^="axis-spine-"] path,
   g[id^="xtick_"] path,
   g[id^="xtick_"] use,
   g[id^="ytick_"] path,
   g[id^="ytick_"] use { stroke: #8c959f !important; }
 }
 </style>"""
    svg = svg[: svg_tag_end + 1] + additions + svg[svg_tag_end + 1 :]
    svg = "\n".join(line.rstrip() for line in svg.splitlines()) + "\n"
    path.write_text(svg, encoding="utf-8", newline="\n")


def create_plot(
    input_path: Path, output_path: Path, png_path: Path | None = None
) -> None:
    results = parse_results(input_path)
    configurations = validate_results(results)

    plt.rcParams.update(
        {
            "font.family": "DejaVu Sans",
            "svg.fonttype": "none",
            "text.color": TEXT,
            "axes.labelcolor": TEXT,
            "axes.titlecolor": TEXT,
        }
    )

    figure, axis = plt.subplots(1, 1, figsize=(12, 6.8))
    figure.patch.set_alpha(0)
    figure.suptitle(
        "Queue throughput on Windows",
        fontsize=18,
        fontweight="bold",
        color=TEXT,
        y=0.985,
    )
    figure.text(
        0.5,
        0.938,
        "MSVC 19.44 · Release · Google Benchmark · 2026-07-29 · "
        "single-item and batch-64 bulk · higher is better",
        ha="center",
        color=MUTED,
        fontsize=9.5,
    )

    plot_all_series(axis, configurations, results)
    style_axis(axis, 0)

    figure.text(
        0.5,
        0.018,
        "Queue construction and worker-thread creation are excluded from timing; "
        "every run validates item count and checksum.",
        ha="center",
        color=MUTED,
        fontsize=8.5,
    )
    figure.subplots_adjust(left=0.09, right=0.74, top=0.86, bottom=0.13)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(
        output_path,
        format="svg",
        transparent=True,
        metadata={
            "Title": "Queue throughput by producer and consumer configuration",
            "Description": "One line per queue on a logarithmic throughput axis.",
        },
    )
    inject_accessibility_and_dark_theme(output_path)

    if png_path is not None:
        png_path.parent.mkdir(parents=True, exist_ok=True)
        figure.savefig(png_path, dpi=180, transparent=False, facecolor="white")

    plt.close(figure)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Google Benchmark JSON file")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("docs/benchmark-throughput.svg"),
        help="output SVG path",
    )
    parser.add_argument("--png", type=Path, help="optional PNG preview path")
    arguments = parser.parse_args()

    create_plot(arguments.input, arguments.output, arguments.png)


if __name__ == "__main__":
    main()
