#!/usr/bin/env python3
"""Plot queue throughput from Google Benchmark JSON output."""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path

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
    Series(
        "BM_HakleImplicitEqualBlocks",
        "Original Hakle implicit",
        "single",
        "#56B4E9",
        "o",
        ":",
    ),
    Series(
        "BM_HakleOptimizedImplicitEqualBlocks",
        "Optimized Hakle implicit",
        "single",
        "#0072B2",
        "o",
        "-",
    ),
    Series(
        "BM_MoodycamelImplicitEqualBlocks",
        "Moodycamel implicit",
        "single",
        "#4C78A8",
        "^",
        "--",
    ),
    Series(
        "BM_HakleTokensEqualBlocks",
        "Original Hakle token",
        "single",
        "#8BC99B",
        "s",
        ":",
    ),
    Series(
        "BM_MoodycamelTokensEqualBlocks",
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
        "BM_HakleTokenBulkEqualBlocks",
        "Original Hakle token bulk",
        "bulk",
        "#9C89B8",
        "*",
        ":",
    ),
    Series(
        "BM_MoodycamelTokenBulkEqualBlocks",
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


def parse_results(
    path: Path,
) -> tuple[
    dict[str, dict[tuple[int, int], float]], dict[str, object], int
]:
    with path.open(encoding="utf-8") as stream:
        document = json.load(stream)

    known = {series.benchmark for series in SERIES}
    results: dict[str, dict[tuple[int, int], float]] = {
        benchmark: {} for benchmark in known
    }

    rows = document.get("benchmarks", [])
    median_runs = {
        row.get("run_name")
        for row in rows
        if row.get("run_type") == "aggregate"
        and row.get("aggregate_name") == "median"
    }

    for row in rows:
        if row.get("run_type") == "aggregate":
            if row.get("aggregate_name") != "median":
                continue
            name = row.get("run_name", row.get("name", ""))
        else:
            name = row.get("name", "")
            if name in median_runs:
                continue

        benchmark = name.split("/", 1)[0]
        match = ARGUMENTS.search(name)
        throughput = row.get("items_per_second")
        if benchmark not in known or match is None or throughput is None:
            continue

        configuration = (int(match.group(1)), int(match.group(2)))
        results[benchmark][configuration] = float(throughput) / 1_000_000.0

    repetitions = max(
        (
            int(row.get("repetitions", 1))
            for row in rows
            if row.get("run_type") == "aggregate"
            and row.get("aggregate_name") == "median"
        ),
        default=1,
    )
    return results, document.get("context", {}), repetitions


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


def style_axis(axis: plt.Axes, axis_index: int) -> None:
    axis.set_facecolor("none")
    axis.grid(color=GRID, linewidth=0.8)
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
            label=series.label,
        )[0]
        line.set_gid(f"series-{series.benchmark}")

    axis.set_yscale("log")
    axis.set_ylim(1.5, 2_000)
    axis.set_yticks([2, 5, 10, 20, 50, 100, 200, 500, 1_000, 2_000])
    axis.set_yticklabels(
        ["2", "5", "10", "20", "50", "100", "200", "500", "1,000", "2,000"]
    )
    axis.set_xticks(
        x_values,
        [f"{producers}P/{consumers}C" for producers, consumers in configurations],
    )
    axis.set_xlim(-0.12, len(configurations) - 1 + 0.12)
    axis.set_ylabel("Throughput (million items/s, log scale)", color=TEXT)
    axis.set_xlabel("Producer / consumer threads", color=MUTED)
    axis.legend(
        loc="center left",
        bbox_to_anchor=(1.04, 0.5),
        frameon=False,
        fontsize=9.5,
        handlelength=3.4,
        labelspacing=1.0,
    )


def inject_accessibility_and_dark_theme(path: Path, title: str) -> None:
    svg = path.read_text(encoding="utf-8")
    svg = svg.replace(
        "<svg ",
        '<svg role="img" aria-labelledby="chart-title chart-desc" ',
        1,
    )
    svg_tag_end = svg.index(">", svg.index("<svg"))
    additions = """
 <title id="chart-title">{title}</title>
 <desc id="chart-desc">Each line represents one queue implementation or calling mode, including the original and optimized Hakle paths. The horizontal axis shows balanced producer and consumer configurations from 1P/1C through 24P/24C. The vertical axis shows median throughput in millions of items per second on a logarithmic scale so single-item and bulk operations remain visible together.</desc>
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
    additions = additions.replace("{title}", title)
    svg = svg[: svg_tag_end + 1] + additions + svg[svg_tag_end + 1 :]
    svg = "\n".join(line.rstrip() for line in svg.splitlines()) + "\n"
    path.write_text(svg, encoding="utf-8", newline="\n")


def create_plot(
    input_path: Path, output_path: Path, png_path: Path | None = None
) -> None:
    results, context, repetitions = parse_results(input_path)
    configurations = validate_results(results)
    logical_cpu_count = context.get("num_cpus")
    build_type = str(context.get("library_build_type", "release")).title()
    configuration = f"Windows · MSVC 19.44 · {build_type}"
    if logical_cpu_count:
        configuration += f" · {logical_cpu_count} logical CPUs"
    title = (
        f"Broad Queue Throughput · Median of {repetitions} Runs "
        f"({configuration})"
    )

    plt.rcParams.update(
        {
            "font.family": "DejaVu Sans",
            "svg.fonttype": "none",
            "text.color": TEXT,
            "axes.labelcolor": TEXT,
            "axes.titlecolor": TEXT,
        }
    )

    figure, axis = plt.subplots(1, 1, figsize=(13.6, 7.2))
    figure.patch.set_alpha(0)
    figure.suptitle(
        title,
        fontsize=18,
        fontweight="normal",
        color=TEXT,
        y=0.955,
    )
    plot_all_series(axis, configurations, results)
    style_axis(axis, 0)

    figure.subplots_adjust(left=0.08, right=0.68, top=0.87, bottom=0.14)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(
        output_path,
        format="svg",
        transparent=True,
        metadata={
            "Title": title,
            "Description": "Original Hakle, Optimized Hakle, moodycamel, Boost.Lockfree, oneTBB, and mutex queue throughput with a logarithmic axis.",
        },
    )
    inject_accessibility_and_dark_theme(output_path, title)

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
