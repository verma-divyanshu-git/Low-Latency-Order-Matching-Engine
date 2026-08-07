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


MAXIMUM_RATES = 32
MAXIMUM_RATE = 1_000_000_000

REQUIRED_FIELDS = {
    "schema_version",
    "mode",
    "scenario",
    "count",
    "valid_samples",
    "executed_operations",
    "invalid_samples",
    "backward_samples",
    "migration_samples",
    "p50_ns",
    "p99_ns",
    "p99_9_ns",
    "mean_ns",
    "requested_rate",
    "achieved_completion_rate",
    "max_backlog",
    "max_lateness_ns",
    "claim_scope",
    "operation_resolution_reason",
    "effective_granularity_ns",
}


def validate_rates(rates: list[int]) -> list[int]:
    if not rates:
        raise ValueError("at least one rate is required")
    if len(rates) > MAXIMUM_RATES:
        raise ValueError(f"at most {MAXIMUM_RATES} rates are allowed")
    if len(set(rates)) != len(rates):
        raise ValueError("rates must be unique")
    if any(type(rate) is not int or rate <= 0 or rate > MAXIMUM_RATE for rate in rates):
        raise ValueError(f"rates must be integers from 1 through {MAXIMUM_RATE}")
    return rates


def _reject_nonfinite_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON number: {value}")


def _require_finite_floats(value: Any, path: str = "summary") -> None:
    if type(value) is float and not math.isfinite(value):
        raise ValueError(f"{path}: floating-point value must be finite")
    if isinstance(value, dict):
        for key, child in value.items():
            _require_finite_floats(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _require_finite_floats(child, f"{path}[{index}]")


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
    data = json.loads(
        path.read_text(encoding="utf-8"), parse_constant=_reject_nonfinite_constant
    )
    if not isinstance(data, dict):
        raise ValueError(f"{path}: summary must be a JSON object")
    _require_finite_floats(data)
    missing = REQUIRED_FIELDS.difference(data)
    if missing:
        raise ValueError(f"{path}: missing fields: {', '.join(sorted(missing))}")
    if type(data["schema_version"]) is not int or data["schema_version"] != 1:
        raise ValueError(f"{path}: unsupported schema")
    if data["mode"] not in {"open-loop"}:
        raise ValueError(f"{path}: unsupported schema or mode")
    if data["claim_scope"] not in {"regression_only", "publishable_candidate"}:
        raise ValueError(f"{path}: invalid claim_scope")
    if data["operation_resolution_reason"] not in {
        "qualified",
        "operation_below_resolution",
    }:
        raise ValueError(f"{path}: invalid operation_resolution_reason")
    integer_fields = (
        "count",
        "valid_samples",
        "executed_operations",
        "invalid_samples",
        "backward_samples",
        "migration_samples",
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
    mean_ns = data["mean_ns"]
    if isinstance(mean_ns, bool) or not isinstance(mean_ns, (int, float)) or mean_ns < 0:
        raise ValueError(f"{path}: mean_ns must be finite and non-negative")
    if data["requested_rate"] == 0 or data["requested_rate"] > MAXIMUM_RATE:
        raise ValueError(f"{path}: requested_rate is out of range")
    if data["count"] != data["valid_samples"]:
        raise ValueError(f"{path}: count must equal valid_samples")
    if data["count"] > data["executed_operations"]:
        raise ValueError(f"{path}: count cannot exceed executed_operations")
    if data["valid_samples"] + data["invalid_samples"] != data["executed_operations"]:
        raise ValueError(f"{path}: valid and invalid samples must equal executed_operations")
    if data["backward_samples"] + data["migration_samples"] != data["invalid_samples"]:
        raise ValueError(f"{path}: invalid sample categories are inconsistent")
    if data["migration_samples"] != 0:
        raise ValueError(f"{path}: successful open-loop summaries cannot contain migration")
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
        ("max_backlog", "max harness arrival backlog (events)"),
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
    try:
        rates = validate_rates(args.rates)
    except ValueError as error:
        parser.error(str(error))
    args.output_dir.mkdir(parents=True, exist_ok=True)

    summaries = []
    for rate in rates:
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
        "valid_samples",
        "invalid_samples",
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
