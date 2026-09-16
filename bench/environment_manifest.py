"""Environment manifest capture for benchmark runs."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import subprocess
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from .telemetry import datadir_disk, sample_cpufreq


def _run_command(cmd: list[str], timeout: int = 10) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        return {"ok": False, "error": str(e), "cmd": cmd}
    return {
        "ok": completed.returncode == 0,
        "returncode": completed.returncode,
        "stdout": completed.stdout.strip(),
        "stderr": completed.stderr.strip(),
        "cmd": cmd,
    }


def _sha256_file(path: Path) -> str | None:
    try:
        digest = hashlib.sha256()
        with path.open("rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def _git_info(repo_path: Path) -> dict[str, Any]:
    rev = _run_command(["git", "-C", str(repo_path), "rev-parse", "HEAD"])
    status = _run_command(
        ["git", "-C", str(repo_path), "status", "--porcelain", "--untracked-files=no"]
    )
    return {
        "commit": rev["stdout"] if rev["ok"] else None,
        "dirty": bool(status["stdout"]) if status["ok"] else None,
        "status_error": status.get("error") or status.get("stderr") or None,
    }


def _file_digest(path: Path) -> dict[str, Any]:
    return {
        "path": str(path),
        "exists": path.exists(),
        "sha256": _sha256_file(path) if path.exists() else None,
    }


def _statvfs(path: Path) -> dict[str, Any]:
    existing = path
    while not existing.exists() and existing != existing.parent:
        existing = existing.parent
    try:
        stat = os.statvfs(existing)
    except OSError as e:
        return {"path": str(path), "error": str(e)}
    return {
        "path": str(path),
        "existing_path": str(existing),
        "block_size": stat.f_frsize,
        "blocks": stat.f_blocks,
        "blocks_free": stat.f_bfree,
        "blocks_available": stat.f_bavail,
    }


def _snapshot_identity(path: Path | None) -> dict[str, Any] | None:
    if path is None:
        return None
    result: dict[str, Any] = {"path": str(path)}
    try:
        stat = path.stat()
        result.update(device=stat.st_dev, inode=stat.st_ino, mtime_ns=stat.st_mtime_ns)
        for name in ("chainstate/CURRENT", "blocks/index/CURRENT"):
            marker = path / name
            result[name] = {
                "contents": marker.read_text().strip() if marker.exists() else None,
                "mtime_ns": marker.stat().st_mtime_ns if marker.exists() else None,
            }
        blocks = path / "blocks"
        result["block_files"] = (
            sum(
                entry.name.startswith(("blk", "rev")) and entry.name.endswith(".dat")
                for entry in blocks.iterdir()
            )
            if blocks.is_dir()
            else None
        )
    except OSError as e:
        result["error"] = str(e)
    return result


def _lsblk() -> dict[str, Any] | None:
    if shutil.which("lsblk") is None:
        return None
    result = _run_command(
        [
            "lsblk",
            "-J",
            "-o",
            "NAME,KNAME,PATH,TYPE,MODEL,SERIAL,SIZE,ROTA,FSTYPE,MOUNTPOINTS",
        ]
    )
    if not result["ok"]:
        return result
    try:
        return json.loads(result["stdout"])
    except json.JSONDecodeError:
        return result


def _smartctl(device: str | None) -> dict[str, Any] | None:
    if device is None or shutil.which("smartctl") is None:
        return None
    path = f"/dev/{device}"
    result = _run_command(["smartctl", "-a", "--json", path], timeout=15)
    if "stdout" not in result:
        return result
    try:
        data = json.loads(result["stdout"])
        data["command_returncode"] = result["returncode"]
        return data
    except json.JSONDecodeError:
        return result


def write_environment_manifest(
    output_file: Path,
    *,
    name: str,
    binary_path: Path,
    binary_commit: str | None,
    source_datadir: Path | None,
    tmp_datadir: Path,
    output_dir: Path,
    bitcoind_command: str,
    hyperfine_command: list[str],
    run_spec: dict[str, Any],
    cache_drop: dict[str, Any],
    fstrim: dict[str, Any],
    repo_path: Path | None = None,
) -> Path:
    """Write a JSON manifest describing the benchmark environment."""
    repo = repo_path or Path.cwd()
    disk = datadir_disk(tmp_datadir)
    device = disk["device"] if disk else None
    data = {
        "created_at": datetime.now(UTC).isoformat(),
        "name": name,
        "benchcoin": {
            "repo": str(repo),
            "git": _git_info(repo),
            "flake_lock": _file_digest(repo / "flake.lock"),
        },
        "bitcoin": {
            "binary": str(binary_path),
            "binary_commit": binary_commit,
            "binary_sha256": _sha256_file(binary_path),
        },
        "commands": {
            "bitcoind": bitcoind_command,
            "hyperfine": hyperfine_command,
        },
        "run_spec": run_spec,
        "paths": {
            "source_datadir": str(source_datadir) if source_datadir else None,
            "tmp_datadir": str(tmp_datadir),
            "output_dir": str(output_dir),
        },
        "snapshot": {
            "identity": _snapshot_identity(source_datadir),
            "source_datadir": _statvfs(source_datadir) if source_datadir else None,
            "tmp_datadir": _statvfs(tmp_datadir),
            "datadir_disk": disk,
        },
        "runner": {
            "hostname": platform.node(),
            "platform": platform.platform(),
            "kernel": platform.release(),
            "uname": platform.uname()._asdict(),
        },
        "cpu": {
            "cpufreq": sample_cpufreq(),
            "lscpu": _run_command(["lscpu", "--json"]),
        },
        "hyperfine_version": _run_command(["hyperfine", "--version"]),
        "disk": {
            "lsblk": _lsblk(),
            "smartctl": _smartctl(device),
        },
        "benchmark_setup": {
            "cache_drop": cache_drop,
            "fstrim": fstrim,
        },
    }
    output_file.write_text(json.dumps(data, indent=2) + "\n")
    return output_file
