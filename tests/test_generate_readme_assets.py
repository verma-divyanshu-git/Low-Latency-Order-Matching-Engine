import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ReadmeAssetTest(unittest.TestCase):
    def test_assets_are_deterministically_generated(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory)
            subprocess.run(
                ["python3", str(ROOT / "scripts/generate_readme_assets.py"), "--output-dir", str(output)],
                check=True,
            )
            for name in ("architecture.svg", "evidence-status.svg"):
                expected = (ROOT / "docs" / "assets" / name).read_bytes()
                actual = (output / name).read_bytes()
                self.assertEqual(actual, expected)
                self.assertIn(b"<title>", actual)
                self.assertIn(b"<desc>", actual)


if __name__ == "__main__":
    unittest.main()
