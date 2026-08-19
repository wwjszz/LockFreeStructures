#!/usr/bin/env python3
"""Render README-style implicit, token, and bulk ablation charts."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from xml.sax.saxutils import escape


SERIES = (
    ("original", "Original Hakle", "#6E7781", "circle", "5 4"),
    ("consumer_cache", "Optimization 1 · consumer cache", "#0072B2", "square", ""),
    ("producer_path", "Optimization 2 · direct dispatch + word flags", "#E69F00", "triangle", ""),
    ("all_optimizations", "All optimizations", "#009E73", "diamond", ""),
    ("moodycamel", "moodycamel", "#D55E00", "cross", "5 4"),
)

PANELS = (
    ("implicit", "Hakle implicit / moodycamel implicit"),
    ("token", "Hakle token / moodycamel token"),
    ("bulk", "Hakle token bulk / moodycamel token bulk · 64 items"),
)


def marker(kind: str, x: float, y: float, color: str, title: str) -> str:
    common = f'fill="{color}" stroke="white" stroke-width="1.2"'
    tooltip = f"<title>{escape(title)}</title>"
    if kind == "circle":
        return f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4.5" {common}>{tooltip}</circle>'
    if kind == "square":
        return f'<rect x="{x - 4.5:.1f}" y="{y - 4.5:.1f}" width="9" height="9" {common}>{tooltip}</rect>'
    if kind == "triangle":
        points = f"{x:.1f},{y - 5.5:.1f} {x - 5:.1f},{y + 4:.1f} {x + 5:.1f},{y + 4:.1f}"
        return f'<polygon points="{points}" {common}>{tooltip}</polygon>'
    if kind == "diamond":
        points = f"{x:.1f},{y - 5.5:.1f} {x - 5.5:.1f},{y:.1f} {x:.1f},{y + 5.5:.1f} {x + 5.5:.1f},{y:.1f}"
        return f'<polygon points="{points}" {common}>{tooltip}</polygon>'
    return (
        f'<g stroke="{color}" stroke-width="2.3">{tooltip}'
        f'<line x1="{x - 4.5:.1f}" y1="{y - 4.5:.1f}" x2="{x + 4.5:.1f}" y2="{y + 4.5:.1f}"/>'
        f'<line x1="{x - 4.5:.1f}" y1="{y + 4.5:.1f}" x2="{x + 4.5:.1f}" y2="{y - 4.5:.1f}"/></g>'
    )


def nice_ceiling(value: float) -> float:
    magnitude = 10 ** math.floor(math.log10(value))
    normalized = value / magnitude
    step = (
        1
        if normalized <= 1
        else 2
        if normalized <= 2
        else 2.5
        if normalized <= 2.5
        else 5
        if normalized <= 5
        else 10
    )
    return step * magnitude


def render(input_path: Path, output_path: Path) -> None:
    payload = json.loads(input_path.read_text(encoding="utf-8"))
    context = payload["context"]
    rows = payload["results"]
    values = {
        (
            row["name"],
            row["path"],
            int(row["producers"]),
            int(row["consumers"]),
        ): float(row["median_million_items_per_second"])
        for row in rows
    }
    configurations = sorted(
        {(int(row["producers"]), int(row["consumers"])) for row in rows},
        key=lambda item: (item[0] + item[1], item[0]),
    )
    missing = [
        (name, path, producers, consumers)
        for name, *_ in SERIES
        for path, _ in PANELS
        for producers, consumers in configurations
        if (name, path, producers, consumers) not in values
    ]
    if missing:
        raise SystemExit(f"missing results: {missing}")

    width, height = 1320, 1080
    left, plot_right = 92, 930
    plot_width = plot_right - left
    panel_height = 226
    panel_tops = (116, 430, 744)
    x_step = plot_width / (len(configurations) - 1)

    cpu = str(context.get("cpu", "unknown CPU"))
    compiler = str(context.get("compiler", "unknown compiler")).replace(
        "Homebrew ", ""
    )
    repetitions = int(context.get("repetitions", 0))
    minimum_seconds = float(context.get("minimum_timed_seconds_per_sample", 0.0))
    title = "ConcurrentQueue optimization ablation · README workloads"
    subtitle = (
        f"{cpu} · {compiler} · median of {repetitions} runs · "
        f"≥ {minimum_seconds:.2f}s timed per sample"
    )

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-labelledby="chart-title chart-desc">',
        f'<title id="chart-title">{escape(title)}</title>',
        '<desc id="chart-desc">Three throughput plots compare the original Hakle queue, two optimization groups, all optimizations, and moodycamel using the README implicit, token, and token bulk workloads.</desc>',
        """<style>
text { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; fill: #24292f; }
.title { font-size: 22px; font-weight: 500; }
.panel-title { font-size: 16px; font-weight: 500; }
.subtitle, .tick, .legend { font-size: 12px; fill: #57606a; }
.axis-label { font-size: 13px; fill: #24292f; }
.grid { stroke: #d0d7de; stroke-width: 1; }
.frame { fill: none; stroke: #8c959f; stroke-width: 1; }
.series { fill: none; stroke-width: 2.3; stroke-linejoin: round; stroke-linecap: round; }
@media (prefers-color-scheme: dark) {
  text, .axis-label { fill: #f0f6fc; }
  .subtitle, .tick, .legend { fill: #b1bac4; }
  .grid { stroke: #30363d; }
  .frame { stroke: #8c959f; }
}
</style>""",
        f'<text class="title" x="{left}" y="34">{escape(title)}</text>',
        f'<text class="subtitle" x="{left}" y="58">{escape(subtitle)}</text>',
    ]

    for panel_index, ((path, panel_title), top) in enumerate(
        zip(PANELS, panel_tops)
    ):
        panel_values = [
            values[(name, path, producers, consumers)]
            for name, *_ in SERIES
            for producers, consumers in configurations
        ]
        y_max = nice_ceiling(max(panel_values) * 1.08)

        def x_position(index: int) -> float:
            return left + index * x_step

        def y_position(value: float) -> float:
            return top + panel_height * (1.0 - value / y_max)

        parts.append(
            f'<text class="panel-title" x="{left}" y="{top - 18}">{escape(panel_title)}</text>'
        )
        tick_count = 5
        for index in range(tick_count + 1):
            value = y_max * index / tick_count
            y = y_position(value)
            parts.append(
                f'<line class="grid" x1="{left}" y1="{y:.1f}" x2="{plot_right}" y2="{y:.1f}"/>'
            )
            parts.append(
                f'<text class="tick" x="{left - 11}" y="{y + 4:.1f}" text-anchor="end">{value:.0f}</text>'
            )
        parts.append(
            f'<rect class="frame" x="{left}" y="{top}" width="{plot_width}" height="{panel_height}"/>'
        )

        for index, (producers, consumers) in enumerate(configurations):
            x = x_position(index)
            parts.append(
                f'<text class="tick" x="{x:.1f}" y="{top + panel_height + 21}" text-anchor="middle">{producers}P/{consumers}C</text>'
            )

        for name, label, color, marker_kind, dash in SERIES:
            points = [
                (
                    x_position(index),
                    y_position(values[(name, path, producers, consumers)]),
                    values[(name, path, producers, consumers)],
                )
                for index, (producers, consumers) in enumerate(configurations)
            ]
            point_text = " ".join(f"{x:.1f},{y:.1f}" for x, y, _ in points)
            dash_attribute = f' stroke-dasharray="{dash}"' if dash else ""
            parts.append(
                f'<polyline class="series" stroke="{color}"{dash_attribute} points="{point_text}"/>'
            )
            for (x, y, value), (producers, consumers) in zip(
                points, configurations
            ):
                parts.append(
                    marker(
                        marker_kind,
                        x,
                        y,
                        color,
                        f"{label} · {panel_title}: {producers}P/{consumers}C · {value:.2f} M items/s",
                    )
                )

        parts.append(
            f'<text class="axis-label" x="{(left + plot_right) / 2:.1f}" y="{top + panel_height + 48}" text-anchor="middle">Producer / consumer threads</text>'
        )
        parts.append(
            f'<text class="axis-label" transform="translate(23 {top + panel_height / 2:.1f}) rotate(-90)" text-anchor="middle">Throughput (million items/s)</text>'
        )

    legend_x = 972
    legend_y = 132
    for index, (_, label, color, marker_kind, dash) in enumerate(SERIES):
        y = legend_y + index * 50
        dash_attribute = f' stroke-dasharray="{dash}"' if dash else ""
        parts.append(
            f'<line x1="{legend_x}" y1="{y}" x2="{legend_x + 32}" y2="{y}" stroke="{color}" stroke-width="2.3"{dash_attribute}/>'
        )
        parts.append(marker(marker_kind, legend_x + 16, y, color, label))
        parts.append(
            f'<text class="legend" x="{legend_x + 43}" y="{y + 4}">{escape(label)}</text>'
        )

    parts.append(
        f'<text class="subtitle" x="{left}" y="{height - 18}">Equal preallocated block count · README thread matrix · validated item count and checksum</text>'
    )
    parts.append("</svg>")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("docs/queue-optimization-ablation.svg"),
    )
    arguments = parser.parse_args()
    render(arguments.input, arguments.output)


if __name__ == "__main__":
    main()
