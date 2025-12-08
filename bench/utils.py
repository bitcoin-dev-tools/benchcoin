"""Utility functions for git, datadir, and system operations."""

from __future__ import annotations

import logging
import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .capabilities import Capabilities
    from .config import Config

logger = logging.getLogger(__name__)


class GitError(Exception):
    """Git operation failed."""

    pass


class GitState:
    """Saved git state for restoration after operations."""

    def __init__(self, repo_path: Path | None = None):
        self.repo_path = repo_path or Path.cwd()
        self.original_branch: str | None = None
        self.original_commit: str | None = None
        self.was_detached: bool = False

    def save(self) -> None:
        """Save current git state."""
        # Check if we're on a branch or detached HEAD
        result = subprocess.run(
            ["git", "symbolic-ref", "--short", "HEAD"],
            capture_output=True,
            text=True,
            cwd=self.repo_path,
        )

        if result.returncode == 0:
            self.original_branch = result.stdout.strip()
            self.was_detached = False
        else:
            # Detached HEAD - save commit hash
            result = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                capture_output=True,
                text=True,
                check=True,
                cwd=self.repo_path,
            )
            self.original_commit = result.stdout.strip()
            self.was_detached = True

        logger.debug(
            f"Saved git state: branch={self.original_branch}, "
            f"commit={self.original_commit}, detached={self.was_detached}"
        )

    def restore(self) -> None:
        """Restore saved git state."""
        if self.original_branch:
            logger.debug(f"Restoring branch: {self.original_branch}")
            subprocess.run(
                ["git", "checkout", self.original_branch],
                check=True,
                cwd=self.repo_path,
            )
        elif self.original_commit:
            logger.debug(f"Restoring detached HEAD: {self.original_commit}")
            subprocess.run(
                ["git", "checkout", self.original_commit],
                check=True,
                cwd=self.repo_path,
            )


def git_checkout(commit: str, repo_path: Path | None = None) -> None:
    """Checkout a specific commit."""
    repo_path = repo_path or Path.cwd()
    logger.info(f"Checking out {commit[:12]}")

    result = subprocess.run(
        ["git", "checkout", commit],
        cwd=repo_path,
        capture_output=True,
        text=True,
    )

    if result.returncode != 0:
        raise GitError(f"Failed to checkout {commit}: {result.stderr}")


def git_rev_parse(ref: str, repo_path: Path | None = None) -> str:
    """Resolve a git reference to a full commit hash."""
    repo_path = repo_path or Path.cwd()

    result = subprocess.run(
        ["git", "rev-parse", ref],
        cwd=repo_path,
        capture_output=True,
        text=True,
    )

    if result.returncode != 0:
        raise GitError(f"Failed to resolve {ref}: {result.stderr}")

    return result.stdout.strip()


def clean_datadir(datadir: Path) -> None:
    """Remove all contents from a data directory."""
    if not datadir.exists():
        return

    logger.debug(f"Cleaning datadir: {datadir}")
    for item in datadir.iterdir():
        if item.is_dir():
            shutil.rmtree(item)
        else:
            item.unlink()


def copy_datadir(src: Path, dst: Path, capabilities: Capabilities) -> None:
    """Copy blockchain data from source to destination."""
    logger.info(f"Copying datadir: {src} -> {dst}")

    # Ensure destination exists
    dst.mkdir(parents=True, exist_ok=True)

    # Build copy command
    cmd = ["cp", "-r"]
    # Copy contents, not directory itself
    cmd += [str(src) + "/.", str(dst)]

    subprocess.run(cmd, check=True)


def drop_caches(capabilities: Capabilities) -> bool:
    """Drop filesystem caches if available.

    Returns True if caches were dropped, False if not available.
    """
    if not capabilities.can_drop_caches or not capabilities.drop_caches_path:
        logger.debug("Cache dropping not available, skipping")
        return False

    logger.debug("Dropping filesystem caches")
    subprocess.run([capabilities.drop_caches_path], check=True)
    return True


def clean_debug_logs(datadir: Path) -> None:
    """Remove debug.log files from datadir and subdirectories."""
    logger.debug(f"Cleaning debug logs in: {datadir}")

    for log_file in datadir.rglob("debug.log"):
        log_file.unlink()


def find_debug_log(datadir: Path) -> Path | None:
    """Find debug.log in datadir or subdirectories."""
    # Check common locations
    candidates = [
        datadir / "debug.log",
        datadir / "mainnet" / "debug.log",
        datadir / "testnet3" / "debug.log",
        datadir / "signet" / "debug.log",
        datadir / "regtest" / "debug.log",
    ]

    for candidate in candidates:
        if candidate.exists():
            return candidate

    # Fallback: search recursively
    for log_file in datadir.rglob("debug.log"):
        return log_file

    return None


def create_temp_script(commands: list[str], name: str = "hook") -> Path:
    """Create a temporary shell script for hyperfine hooks.

    Returns path to the script.
    """
    script_content = "#!/usr/bin/env bash\nset -euxo pipefail\n"
    script_content += "\n".join(commands) + "\n"

    # Create temp file that persists (caller is responsible for cleanup)
    fd, path = tempfile.mkstemp(suffix=".sh", prefix=f"bench_{name}_")
    os.write(fd, script_content.encode())
    os.close(fd)
    os.chmod(path, 0o755)

    return Path(path)


def build_bitcoind_cmd(
    binary: Path,
    datadir: Path,
    config: Config,
    capabilities: Capabilities,
    debug_flags: list[str] | None = None,
) -> list[str]:
    """Build the bitcoind command with optional wrappers.

    Args:
        binary: Path to bitcoind binary
        datadir: Data directory
        config: Benchmark configuration
        capabilities: System capabilities
        debug_flags: Optional debug flags for instrumented mode
    """
    cmd = [
        str(binary),
        f"-datadir={datadir}",
        f"-dbcache={config.dbcache}",
        f"-stopatheight={config.stop_height}",
        "-prune=10000",
        f"-chain={config.chain}",
        "-daemon=0",
        "-printtoconsole=0",
    ]

    # Add connect address if specified
    if config.connect:
        cmd.append(f"-connect={config.connect}")

    # Add debug flags for instrumented mode
    if debug_flags:
        for flag in debug_flags:
            cmd.append(f"-debug={flag}")

    return cmd
