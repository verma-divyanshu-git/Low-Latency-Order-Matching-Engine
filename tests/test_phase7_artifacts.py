import csv
import json
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RESULTS = ROOT / "benchmark-results"


class Phase7ArtifactTest(unittest.TestCase):
    def test_comparisons_are_regression_only_and_checksum_equivalent(self):
        comparison = RESULTS / "phase7-comparison"
        for levels in (64, 4096, 65536):
            data = json.loads((comparison / f"levels-{levels}.json").read_text())
            self.assertEqual(data["schema_version"], 1)
            self.assertEqual(data["claim_scope"], "regression_only")
            self.assertTrue(data["validation_passed"])
            self.assertEqual(data["active_levels"], levels)
            self.assertEqual(len({item["checksum"] for item in data["results"]}), 1)
            self.assertEqual(
                {item["implementation"] for item in data["results"]},
                {"ladder_bitmap", "std_map", "sorted_vector", "absl_btree_map"},
            )

    def test_sweeps_refuse_latency_publication(self):
        paths = (
            RESULTS / "phase7-crossing" / "crossing-limit-rate-sweep.csv",
            RESULTS / "phase7-sweep" / "sweep-3-level-rate-sweep.csv",
        )
        for path in paths:
            with path.open(newline="") as source:
                rows = list(csv.DictReader(source))
            self.assertEqual(len(rows), 6)
            self.assertTrue(all(row["claim_scope"] == "regression_only" for row in rows))
            self.assertTrue(
                all(row["operation_resolution_reason"] == "operation_below_resolution" for row in rows)
            )

    def test_live_host_is_explicitly_nonqualified(self):
        data = json.loads((RESULTS / "phase7-host" / "mac-arm64.json").read_text())
        self.assertEqual(data["evidence_mode"], "live_host")
        self.assertEqual(data["platform"], {"system": "darwin", "architecture": "arm64"})
        self.assertFalse(data["qualified"])

    def test_manifest_forbids_latency_claim(self):
        data = json.loads((RESULTS / "phase7-comparison" / "manifest.json").read_text())
        self.assertEqual(data["claim_scope"], "regression_only")
        self.assertFalse(data["host_qualified"])
        self.assertFalse(data["latency_publishable"])


if __name__ == "__main__":
    unittest.main()
