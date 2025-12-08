"""Build phase - compile bitcoind at specified commits."""

from __future__ import annotations

import logging
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .capabilities import Capabilities
    from .config import Config

from .utils import GitState, git_checkout, git_rev_parse

logger = logging.getLogger(__name__)


@dataclass
class BuildResult:
    """Result of the build phase."""

    base_binary: Path
    head_binary: Path
    base_commit: str
    head_commit: str


class BuildPhase:
    """Build bitcoind binaries at two commits for comparison."""

    def __init__(
        self,
        config: Config,
        capabilities: Capabilities,
        repo_path: Path | None = None,
    ):
        self.config = config
        self.capabilities = capabilities
        self.repo_path = repo_path or Path.cwd()

    def run(
        self,
        base_commit: str,
        head_commit: str,
        binaries_dir: Path | None = None,
    ) -> BuildResult:
        """Build bitcoind at both commits.

        Args:
            base_commit: Git ref for base (comparison) commit
            head_commit: Git ref for head (new) commit
            binaries_dir: Where to store binaries (default: ./binaries)

        Returns:
            BuildResult with paths to built binaries
        """
        # Check prerequisites
        errors = self.capabilities.check_for_build()
        if errors:
            raise RuntimeError("Build prerequisites not met:\n" + "\n".join(errors))

        binaries_dir = binaries_dir or Path(self.config.binaries_dir)

        # Resolve commits to full hashes
        base_hash = git_rev_parse(base_commit, self.repo_path)
        head_hash = git_rev_parse(head_commit, self.repo_path)

        logger.info("Building binaries for comparison:")
        logger.info(f"  Base: {base_hash[:12]} ({base_commit})")
        logger.info(f"  Head: {head_hash[:12]} ({head_commit})")

        # Setup output directories
        base_dir = binaries_dir / "base"
        head_dir = binaries_dir / "head"
        base_dir.mkdir(parents=True, exist_ok=True)
        head_dir.mkdir(parents=True, exist_ok=True)

        base_binary = base_dir / "bitcoind"
        head_binary = head_dir / "bitcoind"

        # Check if we can skip existing builds
        if self.config.skip_existing:
            if base_binary.exists() and head_binary.exists():
                logger.info(
                    "Both binaries exist and --skip-existing set, skipping build"
                )
                return BuildResult(
                    base_binary=base_binary,
                    head_binary=head_binary,
                    base_commit=base_hash,
                    head_commit=head_hash,
                )

        # Save git state for restoration
        git_state = GitState(self.repo_path)
        git_state.save()

        try:
            # Build both commits
            builds = [
                ("base", base_hash, base_binary),
                ("head", head_hash, head_binary),
            ]

            for name, commit, output_path in builds:
                if self.config.skip_existing and output_path.exists():
                    logger.info(f"Skipping {name} build - binary exists")
                    continue

                self._build_commit(name, commit, output_path)

        finally:
            # Always restore git state
            git_state.restore()

        return BuildResult(
            base_binary=base_binary,
            head_binary=head_binary,
            base_commit=base_hash,
            head_commit=head_hash,
        )

    def _build_commit(self, name: str, commit: str, output_path: Path) -> None:
        """Build bitcoind for a single commit."""
        logger.info(f"Building {name} ({commit[:12]})")

        if self.config.dry_run:
            logger.info(f"[DRY RUN] Would build {commit[:12]} -> {output_path}")
            return

        # Checkout the commit
        git_checkout(commit, self.repo_path)

        # Build with nix
        cmd = ["nix", "build", "-L"]

        logger.debug(f"Running: {' '.join(cmd)}")
        result = subprocess.run(
            cmd,
            cwd=self.repo_path,
        )

        if result.returncode != 0:
            raise RuntimeError(f"Build failed for {name} ({commit[:12]})")

        # Copy binary to output location
        nix_binary = self.repo_path / "result" / "bin" / "bitcoind"
        if not nix_binary.exists():
            raise RuntimeError(f"Built binary not found at {nix_binary}")

        # Remove existing binary if present (may be read-only from nix)
        if output_path.exists():
            output_path.chmod(0o755)
            output_path.unlink()

        shutil.copy2(nix_binary, output_path)
        output_path.chmod(0o755)  # Ensure it's executable and writable
        logger.info(f"Built {name} binary: {output_path}")

        # Clean up nix result symlink
        result_link = self.repo_path / "result"
        if result_link.is_symlink():
            result_link.unlink()
