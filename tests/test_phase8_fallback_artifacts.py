import csv
import hashlib
import json
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
FALLBACK = ROOT / "benchmark-results" / "phase8-fallback"


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Phase8FallbackArtifactTest(unittest.TestCase):
    def test_manifest_and_primary_hashes(self):
        manifest = json.loads((FALLBACK / "manifest.json").read_text())
        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["claim_scope"], "regression_only")
        self.assertFalse(manifest["host_qualified"])
        self.assertFalse(manifest["latency_publishable"])
        for section in ("batch", "full_engine"):
            for item in manifest[section]["files"]:
                self.assertEqual(sha256(FALLBACK / item["path"]), item["sha256"])

    def test_independent_batch_runs_are_close_and_equivalent(self):
        manifest = json.loads((FALLBACK / "manifest.json").read_text())
        batch = manifest["batch"]
        self.assertEqual(batch["independent_runs"], 2)
        self.assertEqual(batch["repetitions_per_run"], 21)
        self.assertLess(batch["symmetric_percent_difference"], 5.0)
        runs = [
            json.loads((FALLBACK / "m4-pro-batch-throughput.json").read_text()),
            json.loads((FALLBACK / "m4-pro-batch-throughput-repeat.json").read_text()),
        ]
        self.assertEqual({run["checksum"] for run in runs}, {batch["checksum"]})
        self.assertTrue(all(run["claim_scope"] == "ci_regression_only" for run in runs))
        self.assertTrue(all(run["validation_passed"] for run in runs))

    def test_full_engine_sweeps_are_regression_only(self):
        manifest = json.loads((FALLBACK / "manifest.json").read_text())
        for item in manifest["full_engine"]["files"]:
            with (FALLBACK / item["path"]).open(newline="") as source:
                rows = list(csv.DictReader(source))
            self.assertEqual(len(rows), 5)
            self.assertTrue(all(row["claim_scope"] == "regression_only" for row in rows))
            self.assertTrue(
                all(row["operation_resolution_reason"] == "operation_below_resolution" for row in rows)
            )
            self.assertTrue(all(int(row["invalid_samples"]) == 0 for row in rows))
            self.assertEqual(max(int(row["requested_rate"]) for row in rows), 2_000_000)


if __name__ == "__main__":
    unittest.main()
