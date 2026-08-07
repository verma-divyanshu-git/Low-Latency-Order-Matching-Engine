#!/usr/bin/env python3
"""Run explicit open-loop rates and render dependency-free regression plots."""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
from pathlib import Path
from typing import Any


REQUIRED_FIELDS = {
    "schema_version",
    "mode",
    "scenario",
    "count",
    "executed_operations",
    "p50_ns",
    "p99_ns",
    "p99_9_ns",
    "requested_rate",
    "achieved_completion_rate",
    "max_backlog",
    "max_lateness_ns",
    "claim_scope",
    "operation_resolution_reason",
    "effective_granularity_ns",
}


def escape_xml(value: str) -> str:
    return (
        value.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
        .replace("'", "&apos;")
    )


def load_summary(
    path: Path, expected_scenario: str, expected_rate: int, expected_samples: int
) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    missing = REQUIRED_FIELDS.difference(data)
    if missing:
        raise ValueError(f"{path}: missing fields: {', '.join(sorted(missing))}")
    if data["schema_version"] != 1 or data["mode"] != "open-loop":
        raise ValueError(f"{path}: unsupported schema or mode")
    if data["claim_scope"] not in {"regression_only", "publishable_candidate"}:
        raise ValueError(f"{path}: invalid claim_scope")
    integer_fields = (
        "count",
        "executed_operations",
        "p50_ns",
        "p99_ns",
        "p99_9_ns",
        "requested_rate",
        "max_backlog",
        "max_lateness_ns",
        "effective_granularity_ns",
    )
    for field in integer_fields:
        if type(data[field]) is not int or data[field] < 0:
            raise ValueError(f"{path}: {field} must be a non-negative integer")
    achieved_rate = data["achieved_completion_rate"]
    if isinstance(achieved_rate, bool) or not isinstance(achieved_rate, (int, float)):
        raise ValueError(f"{path}: achieved_completion_rate must be numeric")
    if not math.isfinite(float(achieved_rate)) or achieved_rate < 0:
        raise ValueError(f"{path}: achieved_completion_rate must be finite and non-negative")
    expected = {
        "mode": "open-loop",
        "scenario": expected_scenario,
        "requested_rate": expected_rate,
        "executed_operations": expected_samples,
    }
    for field, value in expected.items():
        if data[field] != value:
            raise ValueError(f"{path}: {field} does not match invocation")
    return data


def _scale(value: float, low: float, high: float, start: float, size: float) -> float:
    if high <= low:
        return start + size / 2
    return start + (value - low) * size / (high - low)


def render_svg(summaries: list[dict[str, Any]], title: str) -> str:
    if not summaries:
        raise ValueError("at least one summary is required")
    width, height = 960, 700
    panels = [
        ("p50_ns", "p50 latency (ns)"),
        ("p99_ns", "p99 latency (ns)"),
        ("p99_9_ns", "p99.9 latency (ns)"),
        ("max_backlog", "max backlog (events)"),
    ]
    rates = [float(item["requested_rate"]) for item in summaries]
    x_low, x_high = min(rates), max(rates)
    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="960" height="700" viewBox="0 0 960 700">',
        "<style>text{font-family:system-ui,sans-serif;fill:#172033}.axis{stroke:#60708a;stroke-width:1}"
        ".line{fill:none;stroke:#1769aa;stroke-width:2}.publishable{fill:#1769aa}"
        ".regression{fill:white;stroke:#c43c39;stroke-width:2;stroke-dasharray:3 2}"
        ".regression-unresolved{fill:#fff4e5;stroke:#b85c00;stroke-width:3}"
        ".note{font-size:12px;fill:#526173}</style>",
        f'<rect width="{width}" height="{height}" fill="#f7f9fc"/>',
        f'<text x="40" y="34" font-size="20">{escape_xml(title)}</text>',
        '<text class="note" x="40" y="55">Solid: candidate; dashed red: regression; '
        "orange: below timer resolution. No saturation conclusion.</text>",
    ]
    for panel_index, (field, label) in enumerate(panels):
        column = panel_index % 2
        row = panel_index // 2
        left = 60 + column * 460
        top = 85 + row * 290
        plot_width, plot_height = 380, 210
        values = [float(item[field]) for item in summaries]
        y_low, y_high = 0.0, max(values)
        if y_high == 0:
            y_high = 1.0
        points = []
        for rate, value in zip(rates, values, strict=True):
            x = _scale(rate, x_low, x_high, left, plot_width)
            y = top + plot_height - _scale(value, y_low, y_high, 0, plot_height)
            points.append((x, y))
        parts.extend(
            [
                f'<text x="{left}" y="{top - 10}" font-size="14">{escape_xml(label)}</text>',
                f'<line class="axis" x1="{left}" y1="{top + plot_height}" '
                f'x2="{left + plot_width}" y2="{top + plot_height}"/>',
                f'<line class="axis" x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_height}"/>',
                f'<polyline class="line" points="{" ".join(f"{x:.2f},{y:.2f}" for x, y in points)}"/>',
                f'<text class="note" x="{left}" y="{top + plot_height + 20}">{x_low:g}</text>',
                f'<text class="note" x="{left + plot_width - 35}" y="{top + plot_height + 20}">{x_high:g}</text>',
                f'<text class="note" x="{left + plot_width / 2 - 45}" y="{top + plot_height + 38}">requested rate</text>',
                f'<text class="note" x="{left + 4}" y="{top + 13}">{y_high:g}</text>',
            ]
        )
        for (x, y), summary in zip(points, summaries, strict=True):
            if summary["operation_resolution_reason"] == "operation_below_resolution":
                point_class = "regression-unresolved"
                resolution = "below timer resolution"
            elif summary["claim_scope"] == "regression_only":
                point_class = "regression"
                resolution = "source regression only"
            else:
                point_class = "publishable"
                resolution = "publication candidate"
            tooltip = escape_xml(
                f"{resolution}; quantization {summary['effective_granularity_ns']} ns"
            )
            parts.append(
                f'<circle class="{point_class}" cx="{x:.2f}" cy="{y:.2f}" r="5">'
                f"<title>{tooltip}</title></circle>"
            )
    parts.append("</svg>")
    return "\n".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--scenario", required=True, choices=("crossing-limit", "sweep-3-level"))
    parser.add_argument("--samples", required=True, type=int)
    parser.add_argument("--warmup", required=True, type=int)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("rates", nargs="+", type=int)
    args = parser.parse_args()
    if args.samples <= 0 or args.samples > 1_000_000 or args.warmup <= 0:
        parser.error("samples and warmup must be positive; samples cannot exceed 1000000")
    if any(rate <= 0 for rate in args.rates):
        parser.error("rates must be positive")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    summaries = []
    for rate in args.rates:
        run_dir = args.output_dir / f"rate-{rate}"
        completed = subprocess.run(
            [
                str(args.executable),
                "--mode",
                "open-loop",
                "--scenario",
                args.scenario,
                "--samples",
                str(args.samples),
                "--warmup",
                str(args.warmup),
                "--rate",
                str(rate),
                "--output-dir",
                str(run_dir),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        output_lines = completed.stdout.splitlines()
        if not output_lines:
            raise ValueError(f"rate {rate}: benchmark did not report a summary path")
        summary_path = Path(output_lines[-1])
        if run_dir.resolve() not in summary_path.resolve().parents:
            raise ValueError(f"rate {rate}: summary path escaped the requested output directory")
        summaries.append(load_summary(summary_path, args.scenario, rate, args.samples))

    csv_path = args.output_dir / f"{args.scenario}-rate-sweep.csv"
    fields = [
        "requested_rate",
        "achieved_completion_rate",
        "count",
        "executed_operations",
        "p50_ns",
        "p99_ns",
        "p99_9_ns",
        "max_backlog",
        "max_lateness_ns",
        "claim_scope",
        "operation_resolution_reason",
        "effective_granularity_ns",
    ]
    with csv_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(summaries)
    svg_path = args.output_dir / f"{args.scenario}-rate-sweep.svg"
    svg_path.write_text(render_svg(summaries, f"{args.scenario} explicit rate sweep"), encoding="utf-8")
    print(csv_path)
    print(svg_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
