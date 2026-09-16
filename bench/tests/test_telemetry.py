from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

from bench.telemetry import main


class TelemetryTests(unittest.TestCase):
    def test_command_writes_metrics_file(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            run_index_file = root / "run-index"
            run_index_file.write_text("7\n")

            status = main(
                [
                    "--output-dir",
                    str(root),
                    "--name",
                    "head-450",
                    "--run-index-file",
                    str(run_index_file),
                    "--datadir",
                    str(root),
                    "--",
                    sys.executable,
                    "-c",
                    "print('ok')",
                ]
            )

            self.assertEqual(status, 0)
            metrics_file = root / "head-450-run-7-metrics.jsonl"
            samples = [
                json.loads(line)
                for line in metrics_file.read_text().splitlines()
                if line.strip()
            ]
            self.assertGreaterEqual(len(samples), 1)
            self.assertIn("timestamp", samples[0])
            self.assertIn("elapsed_s", samples[0])
            self.assertIn("process", samples[0])
            self.assertIn("host", samples[0])

    def test_command_exit_status_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            run_index_file = root / "run-index"
            run_index_file.write_text("1\n")

            status = main(
                [
                    "--output-dir",
                    str(root),
                    "--name",
                    "head-450",
                    "--run-index-file",
                    str(run_index_file),
                    "--datadir",
                    str(root),
                    "--",
                    sys.executable,
                    "-c",
                    "raise SystemExit(23)",
                ]
            )

            self.assertEqual(status, 23)


if __name__ == "__main__":
    unittest.main()
