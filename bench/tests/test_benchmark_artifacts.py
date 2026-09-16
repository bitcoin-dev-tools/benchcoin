from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from bench.artifact_store import ArtifactStore, RunArtifactRecord
from bench.benchmark import BenchmarkPhase
from bench.environment import BenchmarkEnvironment
from bench.run_spec import RunSpec


class BenchmarkArtifactTests(unittest.TestCase):
    def test_manifest_lists_repetition_logs(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            store = ArtifactStore(root)
            logs = [root / f"run-{index}-debug.log" for index in (1, 2)]
            store.write_manifest(
                runs=[
                    RunArtifactRecord(
                        subject="master",
                        profile="450",
                        config={},
                        output_dir=root,
                        results_file=root / "results.json",
                        debug_log=logs[-1],
                        debug_logs=logs,
                    )
                ],
                comparisons=[],
            )

            self.assertEqual(store.load_runs()[0].debug_logs, logs)

    def test_cleanup_preserves_each_repetition_log(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            datadir = root / "datadir"
            output_dir = root / "output"
            datadir.mkdir()
            output_dir.mkdir()
            phase = BenchmarkPhase(
                BenchmarkEnvironment(tmp_datadir=datadir),
                SimpleNamespace(can_fstrim=False, can_drop_caches=False),
                RunSpec(full_ibd=True, start_height=0, runs=2, dbcache=450),
            )

            run_index_file = output_dir / "master-450-run-index"
            setup = phase._create_setup_script(datadir, run_index_file)
            prepare = phase._create_prepare_script(
                datadir, None, "master-450", output_dir, run_index_file
            )
            cleanup = phase._create_cleanup_script(
                datadir, "master-450", output_dir, run_index_file
            )
            subprocess.run([setup], check=True, capture_output=True)
            for number in (1, 2):
                subprocess.run([prepare], check=True, capture_output=True)
                (datadir / "debug.log").write_text(f"run {number}\n")
            subprocess.run([cleanup], check=True, capture_output=True)

            self.assertEqual(
                [
                    (output_dir / f"master-450-run-{number}-debug.log").read_text()
                    for number in (1, 2)
                ],
                ["run 1\n", "run 2\n"],
            )
