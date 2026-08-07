import json
import tempfile
import unittest
from pathlib import Path

from scripts.run_rate_sweep import escape_xml, load_summary, render_svg


class RateSweepTest(unittest.TestCase):
    @staticmethod
    def summary():
        return {
            "schema_version": 1,
            "mode": "open-loop",
            "scenario": "crossing-limit",
            "count": 10,
            "executed_operations": 10,
            "p50_ns": 10,
            "p99_ns": 20,
            "p99_9_ns": 30,
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
