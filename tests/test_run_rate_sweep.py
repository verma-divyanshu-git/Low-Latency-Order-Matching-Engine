import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts.run_rate_sweep import escape_xml, load_summary, main, render_svg, validate_rates


class RateSweepTest(unittest.TestCase):
    @staticmethod
    def summary():
        return {
            "schema_version": 1,
            "mode": "open-loop",
            "scenario": "crossing-limit",
            "count": 10,
            "valid_samples": 10,
            "executed_operations": 10,
            "invalid_samples": 0,
            "backward_samples": 0,
            "migration_samples": 0,
            "p50_ns": 10,
            "p99_ns": 20,
            "p99_9_ns": 30,
            "mean_ns": 12.5,
            "requested_rate": 1000,
            "achieved_completion_rate": 999.0,
            "max_backlog": 0,
            "max_lateness_ns": 2,
            "claim_scope": "regression_only",
            "operation_resolution_reason": "operation_below_resolution",
            "effective_granularity_ns": 41,
        }

    def test_escape_xml_is_deterministic(self):
        self.assertEqual(escape_xml('<a x="1">&\''), "&lt;a x=&quot;1&quot;&gt;&amp;&apos;")

    def test_load_summary_rejects_missing_schema_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "summary.json"
            path.write_text(json.dumps({"schema_version": 1}), encoding="utf-8")
            with self.assertRaises(ValueError):
                load_summary(path, "crossing-limit", 1000, 10)

    def test_load_summary_rejects_bool_float_and_invocation_mismatch(self):
        hostile_fields = (
            "count",
            "executed_operations",
            "requested_rate",
            "max_backlog",
            "max_lateness_ns",
        )
        for field in hostile_fields:
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                summary = self.summary()
                summary[field] = True if field == "count" else 1.5
                path = Path(directory) / "summary.json"
                path.write_text(json.dumps(summary), encoding="utf-8")
                with self.assertRaises(ValueError):
                    load_summary(path, "crossing-limit", 1000, 10)

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "summary.json"
            path.write_text(json.dumps(self.summary()), encoding="utf-8")
            with self.assertRaises(ValueError):
                load_summary(path, "sweep-3-level", 1000, 10)
            with self.assertRaises(ValueError):
                load_summary(path, "crossing-limit", 2000, 10)
            with self.assertRaises(ValueError):
                load_summary(path, "crossing-limit", 1000, 11)

    def test_load_summary_rejects_invalid_enums_accounting_and_nonfinite_json(self):
        mutations = (
            ("mode", "closed-loop-diagnostic"),
            ("claim_scope", "diagnostic_only"),
            ("operation_resolution_reason", "unknown"),
            ("count", 9),
            ("valid_samples", 9),
            ("invalid_samples", 1),
            ("backward_samples", 1),
            ("mean_ns", float("inf")),
        )
        for field, value in mutations:
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                summary = self.summary()
                summary[field] = value
                path = Path(directory) / "summary.json"
                path.write_text(json.dumps(summary), encoding="utf-8")
                with self.assertRaises(ValueError):
                    load_summary(path, "crossing-limit", 1000, 10)

        with tempfile.TemporaryDirectory() as directory:
            summary = self.summary()
            summary["count"] = 11
            summary["valid_samples"] = 11
            path = Path(directory) / "summary.json"
            path.write_text(json.dumps(summary), encoding="utf-8")
            with self.assertRaises(ValueError):
                load_summary(path, "crossing-limit", 1000, 10)

    def test_validate_rates_bounds_count_duplicates_and_values(self):
        self.assertEqual(validate_rates([1, 1_000_000_000]), [1, 1_000_000_000])
        for rates in (
            [],
            [1] * 33,
            [1, 1],
            [0],
            [-1],
            [1_000_000_001],
        ):
            with self.subTest(rates=rates), self.assertRaises(ValueError):
                validate_rates(rates)

    def test_main_rejects_rates_before_subprocess_or_directory_creation(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "not-created"
            arguments = [
                "run_rate_sweep.py",
                "--executable",
                "benchmark",
                "--scenario",
                "crossing-limit",
                "--samples",
                "10",
                "--warmup",
                "1",
                "--output-dir",
                str(output),
                "1000",
                "1000",
            ]
            with (
                mock.patch.object(sys, "argv", arguments),
                mock.patch("sys.stderr", new=io.StringIO()),
                mock.patch("scripts.run_rate_sweep.subprocess.run") as run_process,
                self.assertRaises(SystemExit),
            ):
                main()
            run_process.assert_not_called()
            self.assertFalse(output.exists())

    def test_render_svg_handles_single_rate_and_marks_regression_only(self):
        summary = self.summary()
        svg = render_svg([summary], "crossing<&")
        self.assertIn("crossing&lt;&amp;", svg)
        self.assertIn('class="regression-unresolved"', svg)
        self.assertIn("below timer resolution", svg)
        self.assertNotIn("nan", svg.lower())
        self.assertNotIn("inf", svg.lower())


if __name__ == "__main__":
    unittest.main()
