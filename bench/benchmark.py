"""Benchmark phase - run hyperfine benchmarks comparing two bitcoind binaries."""

from __future__ import annotations

import logging
import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .capabilities import Capabilities
    from .config import Config


logger = logging.getLogger(__name__)

# Debug flags for instrumented mode
INSTRUMENTED_DEBUG_FLAGS = ["coindb", "leveldb", "bench", "validation"]


@dataclass
class BenchmarkResult:
    """Result of the benchmark phase."""

    results_file: Path
    base_commit: str
    head_commit: str
    instrumented: bool
    flamegraph_base: Path | None = None
    flamegraph_head: Path | None = None
    debug_log_base: Path | None = None
    debug_log_head: Path | None = None


class BenchmarkPhase:
    """Run hyperfine benchmarks comparing two bitcoind binaries."""

    def __init__(
        self,
        config: Config,
        capabilities: Capabilities,
    ):
        self.config = config
        self.capabilities = capabilities
        self._temp_scripts: list[Path] = []

    def run(
        self,
        base_commit: str,
        head_commit: str,
        base_binary: Path,
        head_binary: Path,
        datadir: Path,
        output_dir: Path,
    ) -> BenchmarkResult:
        """Run benchmarks comparing base and head binaries.

        Args:
            base_commit: Git hash of base commit
            head_commit: Git hash of head commit
            base_binary: Path to base bitcoind binary
            head_binary: Path to head bitcoind binary
            datadir: Source datadir with blockchain snapshot
            output_dir: Where to store results

        Returns:
            BenchmarkResult with paths to outputs
        """
        # Check prerequisites
        errors = self.capabilities.check_for_run(self.config.instrumented)
        if errors:
            raise RuntimeError("Benchmark prerequisites not met:\n" + "\n".join(errors))

        # Log warnings about missing optional capabilities
        for warning in self.capabilities.get_warnings():
            logger.warning(warning)

        # Setup directories
        output_dir.mkdir(parents=True, exist_ok=True)
        tmp_datadir = Path(self.config.tmp_datadir)
        tmp_datadir.mkdir(parents=True, exist_ok=True)

        results_file = output_dir / "results.json"

        logger.info("Starting benchmark")
        logger.info(f"  Output dir: {output_dir}")
        logger.info(f"  Temp datadir: {tmp_datadir}")
        logger.info(f"  Source datadir: {datadir}")
        logger.info(f"  Base: {base_commit[:12]}")
        logger.info(f"  Head: {head_commit[:12]}")
        logger.info(f"  Instrumented: {self.config.instrumented}")
        logger.info(f"  Runs: {self.config.runs}")
        logger.info(f"  Stop height: {self.config.stop_height}")
        logger.info(f"  dbcache: {self.config.dbcache}")

        try:
            # Create hook scripts for hyperfine
            setup_script = self._create_setup_script(tmp_datadir)
            prepare_script = self._create_prepare_script(tmp_datadir, datadir)
            cleanup_script = self._create_cleanup_script(tmp_datadir)

            # Build hyperfine command
            cmd = self._build_hyperfine_cmd(
                base_commit=base_commit,
                head_commit=head_commit,
                base_binary=base_binary,
                head_binary=head_binary,
                tmp_datadir=tmp_datadir,
                results_file=results_file,
                setup_script=setup_script,
                prepare_script=prepare_script,
                cleanup_script=cleanup_script,
                output_dir=output_dir,
            )

            # Log the commands being benchmarked
            base_cmd = self._build_bitcoind_cmd(base_binary, tmp_datadir)
            head_cmd = self._build_bitcoind_cmd(head_binary, tmp_datadir)
            logger.info("Base command:")
            logger.info(f"  {base_cmd}")
            logger.info("Head command:")
            logger.info(f"  {head_cmd}")

            if self.config.dry_run:
                logger.info(f"[DRY RUN] Would run: {' '.join(cmd)}")
                return BenchmarkResult(
                    results_file=results_file,
                    base_commit=base_commit,
                    head_commit=head_commit,
                    instrumented=self.config.instrumented,
                )

            # Log the full hyperfine command
            logger.info("Running hyperfine...")
            logger.info(f"  Command: {' '.join(cmd[:7])} ...")  # First few args
            logger.debug(f"  Full command: {' '.join(cmd)}")
            _result = subprocess.run(cmd, check=True)

            # Collect results
            benchmark_result = BenchmarkResult(
                results_file=results_file,
                base_commit=base_commit,
                head_commit=head_commit,
                instrumented=self.config.instrumented,
            )

            # For instrumented runs, collect flamegraphs and debug logs
            if self.config.instrumented:
                logger.info("Collecting instrumented artifacts...")
                base_fg = output_dir / f"{base_commit[:12]}-flamegraph.svg"
                head_fg = output_dir / f"{head_commit[:12]}-flamegraph.svg"
                base_log = output_dir / f"{base_commit[:12]}-debug.log"
                head_log = output_dir / f"{head_commit[:12]}-debug.log"

                # Move flamegraphs from current directory if they exist
                for src_name, dest in [
                    ("base-flamegraph.svg", base_fg),
                    ("head-flamegraph.svg", head_fg),
                ]:
                    src = Path(src_name)
                    if src.exists():
                        logger.info(f"  Moving {src_name} -> {dest}")
                        shutil.move(str(src), str(dest))

                if base_fg.exists():
                    benchmark_result.flamegraph_base = base_fg
                    logger.info(f"  Flamegraph (base): {base_fg}")
                if head_fg.exists():
                    benchmark_result.flamegraph_head = head_fg
                    logger.info(f"  Flamegraph (head): {head_fg}")
                if base_log.exists():
                    benchmark_result.debug_log_base = base_log
                    logger.info(f"  Debug log (base): {base_log}")
                if head_log.exists():
                    benchmark_result.debug_log_head = head_log
                    logger.info(f"  Debug log (head): {head_log}")

            # Clean up tmp_datadir
            if tmp_datadir.exists():
                logger.debug(f"Cleaning up tmp_datadir: {tmp_datadir}")
                shutil.rmtree(tmp_datadir)

            return benchmark_result

        finally:
            # Clean up temp scripts
            for script in self._temp_scripts:
                if script.exists():
                    script.unlink()
            self._temp_scripts.clear()

    def _create_temp_script(self, commands: list[str], name: str) -> Path:
        """Create a temporary shell script."""
        content = "#!/usr/bin/env bash\nset -euxo pipefail\n"
        content += "\n".join(commands) + "\n"

        fd, path = tempfile.mkstemp(suffix=".sh", prefix=f"bench_{name}_")
        os.write(fd, content.encode())
        os.close(fd)
        os.chmod(path, 0o755)

        script_path = Path(path)
        self._temp_scripts.append(script_path)
        logger.debug(f"Created {name} script: {script_path}")
        for cmd in commands:
            logger.debug(f"  {cmd}")
        return script_path

    def _create_setup_script(self, tmp_datadir: Path) -> Path:
        """Create setup script (runs once before all timing runs)."""
        commands = [
            f'mkdir -p "{tmp_datadir}"',
            f'rm -rf "{tmp_datadir}"/*',
        ]
        return self._create_temp_script(commands, "setup")

    def _create_prepare_script(self, tmp_datadir: Path, original_datadir: Path) -> Path:
        """Create prepare script (runs before each timing run)."""
        commands = [
            f'rm -rf "{tmp_datadir}"/*',
        ]

        # Copy datadir
        commands.append(f'cp -r "{original_datadir}"/* "{tmp_datadir}"')

        # Drop caches if available
        if self.capabilities.can_drop_caches and not self.config.no_cache_drop:
            commands.append(self.capabilities.drop_caches_path)

        # Clean debug logs
        commands.append(
            f'find "{tmp_datadir}" -name debug.log -delete 2>/dev/null || true'
        )

        return self._create_temp_script(commands, "prepare")

    def _create_cleanup_script(self, tmp_datadir: Path) -> Path:
        """Create cleanup script (runs after all timing runs for each command)."""
        commands = [
            f'rm -rf "{tmp_datadir}"/*',
        ]
        return self._create_temp_script(commands, "cleanup")

    def _build_bitcoind_cmd(
        self,
        binary: Path,
        tmp_datadir: Path,
    ) -> str:
        """Build the bitcoind command string for hyperfine."""
        parts = []

        # Add flamegraph wrapper for instrumented mode
        if self.config.instrumented:
            parts.append("flamegraph")
            parts.append("--palette bitcoin")
            parts.append("--title 'bitcoind IBD'")
            parts.append("-c 'record -F 101 --call-graph fp'")
            parts.append("--")

        # Bitcoind command
        parts.append(str(binary))
        parts.append(f"-datadir={tmp_datadir}")
        parts.append(f"-dbcache={self.config.dbcache}")
        parts.append(f"-stopatheight={self.config.stop_height}")
        parts.append("-prune=10000")
        parts.append(f"-chain={self.config.chain}")
        parts.append("-daemon=0")
        parts.append("-printtoconsole=0")

        if self.config.connect:
            parts.append(f"-connect={self.config.connect}")

        # Debug flags for instrumented mode
        if self.config.instrumented:
            for flag in INSTRUMENTED_DEBUG_FLAGS:
                parts.append(f"-debug={flag}")

        return " ".join(parts)

    def _build_hyperfine_cmd(
        self,
        base_commit: str,
        head_commit: str,
        base_binary: Path,
        head_binary: Path,
        tmp_datadir: Path,
        results_file: Path,
        setup_script: Path,
        prepare_script: Path,
        cleanup_script: Path,
        output_dir: Path,
    ) -> list[str]:
        """Build the hyperfine command."""
        cmd = [
            "hyperfine",
            "--shell=bash",
            f"--setup={setup_script}",
            f"--prepare={prepare_script}",
            f"--cleanup={cleanup_script}",
            f"--runs={self.config.runs}",
            f"--export-json={results_file}",
            "--show-output",
        ]

        # For instrumented runs, we need separate conclude scripts per commit
        # since hyperfine's parameter substitution doesn't work with --conclude
        if self.config.instrumented:
            base_conclude = self._create_conclude_script_for_commit(
                base_commit[:12], tmp_datadir, output_dir
            )
            head_conclude = self._create_conclude_script_for_commit(
                head_commit[:12], tmp_datadir, output_dir
            )
            # We'll handle conclude differently - see below

        # Command names
        cmd.append(f"--command-name=base ({base_commit[:12]})")
        cmd.append(f"--command-name=head ({head_commit[:12]})")

        # Build the actual commands to benchmark
        base_cmd = self._build_bitcoind_cmd(base_binary, tmp_datadir)
        head_cmd = self._build_bitcoind_cmd(head_binary, tmp_datadir)

        # For instrumented runs, append the conclude logic to each command
        if self.config.instrumented:
            base_cmd += f" && {base_conclude}"
            head_cmd += f" && {head_conclude}"

        cmd.append(base_cmd)
        cmd.append(head_cmd)

        return cmd

    def _create_conclude_script_for_commit(
        self,
        commit: str,
        tmp_datadir: Path,
        output_dir: Path,
    ) -> str:
        """Create inline conclude commands for a specific commit."""
        # Return shell commands to run after each benchmark
        commands = []

        # Move flamegraph if exists
        commands.append(f'if [ -e flamegraph.svg ]; then mv flamegraph.svg "{output_dir}/{commit}-flamegraph.svg"; fi')

        # Copy debug log if exists
        commands.append(
            f'debug_log=$(find "{tmp_datadir}" -name debug.log -print -quit); '
            f'if [ -n "$debug_log" ]; then cp "$debug_log" "{output_dir}/{commit}-debug.log"; fi'
        )

        return " && ".join(commands)
