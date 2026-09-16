from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from bench.environment_manifest import write_environment_manifest


class EnvironmentManifestTests(unittest.TestCase):
    def test_records_binary_command_and_snapshot_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            binary = root / "bitcoind"
            binary.write_bytes(b"test binary")
            (root / "flake.lock").write_text("test lock")
            source = root / "snapshot"
            (source / "chainstate").mkdir(parents=True)
            (source / "chainstate" / "CURRENT").write_text("MANIFEST-123\n")
            (source / "blocks").mkdir()
            (source / "blocks" / "blk00000.dat").write_bytes(b"block data")

            output = root / "environment.json"
            write_environment_manifest(
                output,
                name="head-450",
                binary_path=binary,
                binary_commit="abc123",
                source_datadir=source,
                tmp_datadir=root / "datadir",
                output_dir=root,
                bitcoind_command="bitcoind -dbcache=450",
                hyperfine_command=["hyperfine", "bitcoind -dbcache=450"],
                run_spec={"runs": 2},
                cache_drop={"enabled": False},
                fstrim={"enabled": False},
                repo_path=root,
            )

            data = json.loads(output.read_text())
            self.assertEqual(data["bitcoin"]["binary_commit"], "abc123")
            self.assertEqual(len(data["bitcoin"]["binary_sha256"]), 64)
            self.assertEqual(len(data["benchcoin"]["flake_lock"]["sha256"]), 64)
            self.assertEqual(data["snapshot"]["identity"]["block_files"], 1)
            self.assertEqual(
                data["snapshot"]["identity"]["chainstate/CURRENT"]["contents"],
                "MANIFEST-123",
            )
            self.assertEqual(data["commands"]["bitcoind"], "bitcoind -dbcache=450")


if __name__ == "__main__":
    unittest.main()
