import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts import verify_release


class ReleaseGateTest(unittest.TestCase):
    def test_static_claim_and_artifact_contract_passes(self):
        self.assertEqual(verify_release.static_checks(ROOT), [])

    def test_plan_contains_all_required_presets_and_release_outputs(self):
        names = [check.name for check in verify_release.build_plan()]
        for preset in ("debug", "release", "asan", "ubsan", "tsan", "fuzz", "measurement"):
            self.assertIn(f"configure-{preset}", names)
            self.assertIn(f"build-{preset}", names)
            self.assertIn(f"test-{preset}", names)
        self.assertIn("recovery-regressions", names)
        self.assertIn("binary-package", names)
        self.assertIn("source-package", names)

    def test_sha256_is_stable(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "artifact"
            path.write_bytes(b"matching-engine\n")
            self.assertEqual(
                verify_release.sha256(path),
                "ae7d08b5aac92cbfda2fb5d8f2a9a33408c8e350207330a16df6d4dffabad40a",
            )


if __name__ == "__main__":
    unittest.main()
