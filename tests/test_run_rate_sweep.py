import json
import tempfile
import unittest
from pathlib import Path

from scripts.run_rate_sweep import escape_xml, load_summary, render_svg


class RateSweepTest(unittest.TestCase):
    def test_escape_xml_is_deterministic(self):
        self.assertEqual(escape_xml('<a x="1">&\''), "&lt;a x=&quot;1&quot;&gt;&amp;&apos;")

    def test_load_summary_rejects_missing_schema_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "summary.json"
            path.write_text(json.dumps({"schema_version": 1}), encoding="utf-8")
            with self.assertRaises(ValueError):
                load_summary(path)

    def test_render_svg_handles_single_rate_and_marks_regression_only(self):
        summary = {
            "schema_version": 1,
            "mode": "open-loop",
            "scenario": "crossing-limit",
            "count": 10,
            "p50_ns": 10,
            "p99_ns": 20,
            "p99_9_ns": 30,
            "requested_rate": 1000,
            "max_backlog": 2,
            "claim_scope": "regression_only",
        }
        svg = render_svg([summary], "crossing<&")
        self.assertIn("crossing&lt;&amp;", svg)
        self.assertIn('class="regression"', svg)
        self.assertNotIn("nan", svg.lower())
        self.assertNotIn("inf", svg.lower())


if __name__ == "__main__":
    unittest.main()
