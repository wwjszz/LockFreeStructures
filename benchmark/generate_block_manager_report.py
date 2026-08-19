#!/usr/bin/env python3
"""Generate the self-contained BlockManager benchmark HTML report."""

from __future__ import annotations

import argparse
import html
import json
import re
import statistics
from pathlib import Path


THREADS = (1, 2, 4, 8, 12, 16, 20, 24)
QUEUES = ("HakleImplicit", "HakleSlabImplicit", "MoodycamelImplicit")
LABELS = {
    "HakleImplicit": "Hakle（优化前）",
    "HakleSlabImplicit": "Hakle + ShardedSlab32（优化后）",
    "MoodycamelImplicit": "moodycamel",
}
COLORS = {
    "HakleImplicit": "var(--before)",
    "HakleSlabImplicit": "var(--after)",
    "MoodycamelImplicit": "var(--moody)",
}
BENCHMARK_RE = re.compile(
    r"^BM_(?:ProducerOnly)?"
    r"(?P<queue>HakleImplicit|HakleSlabImplicit|MoodycamelImplicit)"
    r"(?P<pool>EqualBlocks|ZeroInitialPool)/producers:(?P<producers>\d+)/"
)


def load_medians(path: Path) -> tuple[dict[tuple[str, str, int], float], dict]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    values: dict[tuple[str, str, int], float] = {}
    for row in payload["benchmarks"]:
        if row.get("run_type") != "aggregate" or row.get("aggregate_name") != "median":
            continue
        match = BENCHMARK_RE.match(row["name"])
        if match is None:
            continue
        key = (
            match.group("queue"),
            match.group("pool"),
            int(match.group("producers")),
        )
        values[key] = float(row["items_per_second"]) / 1_000_000.0
    return values, payload["context"]


def percent(after: float, before: float) -> float:
    return (after / before - 1.0) * 100.0


def percent_text(value: float) -> str:
    return f"{value:+.1f}%"


def delta_class(value: float) -> str:
    if value > 2.0:
        return "positive"
    if value < -2.0:
        return "negative"
    return "neutral"


def make_svg(title: str, values: dict[tuple[str, str, int], float], pool: str) -> str:
    width, height = 820, 360
    left, right, top, bottom = 72, 24, 42, 54
    plot_width = width - left - right
    plot_height = height - top - bottom
    maximum = max(values[(queue, pool, thread)] for queue in QUEUES for thread in THREADS)
    y_max = maximum * 1.10

    def x(thread: int) -> float:
        return left + THREADS.index(thread) * plot_width / (len(THREADS) - 1)

    def y(value: float) -> float:
        return top + plot_height * (1.0 - value / y_max)

    parts = [
        f'<svg class="chart" viewBox="0 0 {width} {height}" role="img" '
        f'aria-label="{html.escape(title)}">',
        f"<title>{html.escape(title)}</title>",
        f'<text class="chart-title" x="{left}" y="24">{html.escape(title)}</text>',
    ]
    for tick in range(6):
        tick_value = y_max * tick / 5
        tick_y = y(tick_value)
        parts.append(
            f'<line class="grid" x1="{left}" y1="{tick_y:.1f}" '
            f'x2="{width-right}" y2="{tick_y:.1f}" />'
        )
        parts.append(
            f'<text class="tick" x="{left-10}" y="{tick_y+4:.1f}" '
            f'text-anchor="end">{tick_value:.0f}</text>'
        )
    parts.append(
        f'<rect class="frame" x="{left}" y="{top}" width="{plot_width}" '
        f'height="{plot_height}" />'
    )
    for thread in THREADS:
        tick_x = x(thread)
        parts.append(
            f'<text class="tick" x="{tick_x:.1f}" y="{height-bottom+22}" '
            f'text-anchor="middle">{thread}</text>'
        )
    parts.append(
        f'<text class="axis-label" x="{left+plot_width/2:.1f}" y="{height-10}" '
        f'text-anchor="middle">生产者线程数</text>'
    )
    parts.append(
        f'<text class="axis-label" transform="translate(18 {top+plot_height/2:.1f}) rotate(-90)" '
        f'text-anchor="middle">吞吐量（M items/s）</text>'
    )

    for queue in QUEUES:
        points = " ".join(
            f"{x(thread):.1f},{y(values[(queue, pool, thread)]):.1f}" for thread in THREADS
        )
        parts.append(
            f'<polyline class="series-line" style="stroke:{COLORS[queue]}" points="{points}" />'
        )
        for thread in THREADS:
            value = values[(queue, pool, thread)]
            parts.append(
                f'<circle class="series-point" style="fill:{COLORS[queue]}" '
                f'cx="{x(thread):.1f}" cy="{y(value):.1f}" r="4">'
                f"<title>{html.escape(LABELS[queue])}: {value:.2f} M items/s</title></circle>"
            )

    legend_x = left
    for queue, offset in zip(QUEUES, (0, 205, 520)):
        lx = legend_x + offset
        parts.append(
            f'<line class="legend-line" style="stroke:{COLORS[queue]}" '
            f'x1="{lx}" y1="{height-28}" x2="{lx+22}" y2="{height-28}" />'
        )
        parts.append(
            f'<text class="legend-text" x="{lx+29}" y="{height-24}">'
            f"{html.escape(LABELS[queue])}</text>"
        )
    parts.append("</svg>")
    return "".join(parts)


def make_table(values: dict[tuple[str, str, int], float], pool: str) -> str:
    rows = []
    for thread in THREADS:
        before = values[("HakleImplicit", pool, thread)]
        after = values[("HakleSlabImplicit", pool, thread)]
        moody = values[("MoodycamelImplicit", pool, thread)]
        vs_before = percent(after, before)
        vs_moody = percent(after, moody)
        rows.append(
            "<tr>"
            f'<td class="number">{thread}</td>'
            f'<td class="number">{before:.2f}</td>'
            f'<td class="number strong">{after:.2f}</td>'
            f'<td class="number {delta_class(vs_before)}">{percent_text(vs_before)}</td>'
            f'<td class="number">{moody:.2f}</td>'
            f'<td class="number {delta_class(vs_moody)}">{percent_text(vs_moody)}</td>'
            "</tr>"
        )
    return "".join(rows)


def main() -> None:
    parser = argparse.ArgumentParser()
    base = Path(__file__).resolve().parent
    parser.add_argument(
        "--producer-only",
        type=Path,
        default=base / "final-out-of-line-slab-producer-only.json",
    )
    parser.add_argument(
        "--mpmc",
        type=Path,
        default=base / "final-out-of-line-slab-mpmc.json",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=base / "block-manager-optimization-report.html",
    )
    args = parser.parse_args()

    producer_values, producer_context = load_medians(args.producer_only)
    mpmc_values, _ = load_medians(args.mpmc)

    producer_deltas = [
        percent(
            producer_values[("HakleSlabImplicit", "ZeroInitialPool", thread)],
            producer_values[("HakleImplicit", "ZeroInitialPool", thread)],
        )
        for thread in THREADS
    ]
    producer_vs_moody = [
        percent(
            producer_values[("HakleSlabImplicit", "ZeroInitialPool", thread)],
            producer_values[("MoodycamelImplicit", "ZeroInitialPool", thread)],
        )
        for thread in THREADS
    ]
    mpmc_deltas = [
        percent(
            mpmc_values[("HakleSlabImplicit", "ZeroInitialPool", thread)],
            mpmc_values[("HakleImplicit", "ZeroInitialPool", thread)],
        )
        for thread in THREADS
    ]

    producer_chart = make_svg(
        "突发深队列：producer-only / 初始池为 0", producer_values, "ZeroInitialPool"
    )
    mpmc_chart = make_svg(
        "普通 MPMC：生产者与消费者并发 / 初始池为 0", mpmc_values, "ZeroInitialPool"
    )
    producer_table = make_table(producer_values, "ZeroInitialPool")
    mpmc_table = make_table(mpmc_values, "ZeroInitialPool")

    generated = html.escape(str(producer_context.get("date", "")))
    cpu_summary = (
        f'{producer_context.get("num_cpus", "?")} logical CPUs × '
        f'{producer_context.get("mhz_per_cpu", "?")} MHz'
    )
    document = f"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Hakle BlockManager 优化 benchmark</title>
<style>
:root {{ color-scheme: light dark; --bg:#f7f8fa; --surface:#fff; --text:#17202a; --muted:#667085; --border:#d9dee7; --before:#64748b; --after:#16a34a; --moody:#2563eb; --positive:#15803d; --negative:#b42318; --code:#eef1f5; }}
@media (prefers-color-scheme: dark) {{ :root {{ --bg:#0f141b; --surface:#171e28; --text:#edf2f7; --muted:#aab4c3; --border:#344050; --before:#a3adc2; --after:#4ade80; --moody:#60a5fa; --positive:#4ade80; --negative:#fb7185; --code:#202938; }} }}
* {{ box-sizing:border-box; }}
body {{ margin:0; background:var(--bg); color:var(--text); font:15px/1.55 system-ui,-apple-system,"Segoe UI",sans-serif; }}
main {{ width:min(1180px,calc(100% - 32px)); margin:32px auto 64px; }}
h1 {{ margin:0 0 8px; font-size:28px; }} h2 {{ margin:32px 0 12px; font-size:21px; }} h3 {{ margin:24px 0 8px; font-size:17px; }}
.subtitle,.note {{ color:var(--muted); }}
.summary {{ display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:12px; margin:22px 0; }}
.metric {{ padding:16px; background:var(--surface); border:1px solid var(--border); border-radius:10px; }}
.metric b {{ display:block; font-size:24px; color:var(--after); }} .metric span {{ color:var(--muted); }}
.callout {{ padding:14px 16px; border-left:4px solid var(--after); background:var(--surface); }}
.charts {{ display:grid; grid-template-columns:1fr 1fr; gap:18px; }}
.chart {{ width:100%; height:auto; background:var(--surface); border:1px solid var(--border); border-radius:10px; }}
.chart-title {{ fill:var(--text); font-size:15px; font-weight:600; }} .tick,.legend-text,.axis-label {{ fill:var(--text); font-size:12px; }}
.grid {{ stroke:var(--border); stroke-width:1; }} .frame {{ fill:none; stroke:var(--border); }} .series-line {{ fill:none; stroke-width:2.5; }} .series-point {{ stroke:var(--surface); stroke-width:1.5; }} .legend-line {{ stroke-width:3; }}
.table-wrap {{ overflow-x:auto; background:var(--surface); border:1px solid var(--border); border-radius:10px; }}
table {{ width:100%; border-collapse:collapse; min-width:760px; }} th,td {{ padding:9px 12px; border-bottom:1px solid var(--border); text-align:left; }} th {{ color:var(--muted); font-weight:600; }} tr:last-child td {{ border-bottom:0; }}
.number {{ text-align:right; font-variant-numeric:tabular-nums; }} .strong {{ font-weight:700; }} .positive {{ color:var(--positive); font-weight:700; }} .negative {{ color:var(--negative); font-weight:700; }} .neutral {{ color:var(--muted); }}
code {{ background:var(--code); padding:2px 5px; border-radius:4px; }}
ul {{ padding-left:22px; }} .meta {{ margin-top:28px; color:var(--muted); font-size:13px; }}
@media (max-width:850px) {{ .summary,.charts {{ grid-template-columns:1fr; }} main {{ width:min(100% - 20px,1180px); margin-top:20px; }} }}
</style>
</head>
<body><main>
<h1>Hakle BlockManager 优化 benchmark</h1>
<p class="subtitle">优化前：当前默认 <code>HakleBlockManager</code>；优化后：仅为 implicit/Counter Block 选择 <code>SlabBlockManager&lt;..., 32, 32&gt;</code>。默认 traits 未改变。</p>
<div class="summary">
  <div class="metric"><span>深队列相对优化前</span><b>{statistics.median(producer_deltas):+.1f}%</b><span>8 个线程点的提升中位数（范围 {min(producer_deltas):+.1f}% ~ {max(producer_deltas):+.1f}%）</span></div>
  <div class="metric"><span>深队列相对 moodycamel</span><b>{statistics.median(producer_vs_moody):+.1f}%</b><span>提升中位数（范围 {min(producer_vs_moody):+.1f}% ~ {max(producer_vs_moody):+.1f}%）</span></div>
  <div class="metric"><span>普通 MPMC 相对优化前</span><b>{statistics.median(mpmc_deltas):+.1f}%</b><span>基本持平（范围 {min(mpmc_deltas):+.1f}% ~ {max(mpmc_deltas):+.1f}%）</span></div>
</div>
<div class="callout"><strong>结论：</strong>这是面向“零初始池 + 突发深队列”的可选策略，不是新的通用默认值。它减少动态分配调用并保留 slab 供后续复用；普通浅队列几乎不受影响，但会增加保留内存。</div>

<h2>最终组合曲线</h2>
<div class="charts">{producer_chart}{mpmc_chart}</div>

<h2>优化 1：Out-of-line Sharded Slab32</h2>
<ul>
  <li>32 个 shard；线程第一次访问 manager 时按轮转分配 shard，避免多个 producer 争用同一个 slab 慢路径。</li>
  <li>每个 slab 连续申请 32 个 Block；剩余新 Block 走一次性 fresh stack，使用后的 Block 仍归还到原有安全 FreeList。</li>
  <li>shard 的 mutex/原子状态由传入 allocator 的 rebind 分配在 manager 外部，避免把队列热对象放大数 KB。</li>
  <li>slab 由 manager 持有到析构：适合突发、深度增长和反复复用；代价是每个活跃 shard 最多保留 31 个暂未使用的 Block。</li>
</ul>

<h3>突发深队列：producer-only，初始池为 0</h3>
<div class="table-wrap"><table>
<thead><tr><th class="number">生产者</th><th class="number">优化前 M/s</th><th class="number">优化后 M/s</th><th class="number">相对优化前</th><th class="number">moodycamel M/s</th><th class="number">相对 moodycamel</th></tr></thead>
<tbody>{producer_table}</tbody></table></div>

<h3>普通 MPMC：N producer + N consumer，初始池为 0</h3>
<div class="table-wrap"><table>
<thead><tr><th class="number">P / C</th><th class="number">优化前 M/s</th><th class="number">优化后 M/s</th><th class="number">相对优化前</th><th class="number">moodycamel M/s</th><th class="number">相对 moodycamel</th></tr></thead>
<tbody>{mpmc_table}</tbody></table></div>

<h2>没有保留的实验</h2>
<div class="table-wrap"><table>
<thead><tr><th>实验</th><th>结果</th><th>处理</th></tr></thead><tbody>
<tr><td>单全局 Slab256</td><td>多 producer 争用同一个 fresh-block 栈，深队列明显退化</td><td>改为分片</td></tr>
<tr><td>slab size 8 / 128</td><td>同场景均弱于 32；8 的 allocator 调用更多，128 的批量构造延迟更高</td><td>最终选择 32</td></tr>
<tr><td>元数据与 Block 合并为一次分配</td><td>allocator 调用减少，但实际吞吐下降</td><td>已回退</td></tr>
<tr><td>Slab 用于 Flags/显式 token</td><td>中等并发收益不稳定并出现负值</td><td>不推荐组合；最终只用于 implicit/Counter</td></tr>
</tbody></table></div>

<p class="meta">Release；Google Benchmark {html.escape(str(producer_context.get("library_version", "")))}；{html.escape(cpu_summary)}；producer-only 11 次随机交错，MPMC 7 次随机交错；中位数。生成时间：{generated}。原始数据：<code>{html.escape(args.producer_only.name)}</code>、<code>{html.escape(args.mpmc.name)}</code>。</p>
</main></body></html>
"""
    args.output.write_text(document, encoding="utf-8")
    print(args.output.resolve())


if __name__ == "__main__":
    main()
