"""Low-rate host and process telemetry for benchmark repetitions."""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from collections.abc import Callable
from datetime import UTC, datetime
from pathlib import Path
from typing import Any


def _sysconf(name: str, default: int) -> int:
    try:
        return int(os.sysconf(name))
    except (OSError, ValueError, AttributeError):
        return default


CLK_TCK = _sysconf("SC_CLK_TCK", 100)
PAGE_SIZE = _sysconf("SC_PAGE_SIZE", 4096)
DISKSTAT_FIELDS = [
    "reads_completed",
    "reads_merged",
    "sectors_read",
    "read_ms",
    "writes_completed",
    "writes_merged",
    "sectors_written",
    "write_ms",
    "ios_in_progress",
    "io_ms",
    "weighted_io_ms",
    "discards_completed",
    "discards_merged",
    "sectors_discarded",
    "discard_ms",
    "flushes_completed",
    "flush_ms",
]


def _read_text(path: Path) -> str | None:
    try:
        return path.read_text().strip()
    except OSError:
        return None


def _read_int(path: Path) -> int | None:
    text = _read_text(path)
    if text is None:
        return None
    try:
        return int(text)
    except ValueError:
        return None


def _parse_proc_stat(text: str) -> dict[str, int] | None:
    end = text.rfind(")")
    if end == -1:
        return None
    fields = text[end + 2 :].split()
    if len(fields) < 22:
        return None
    try:
        return {
            "ppid": int(fields[1]),
            "utime_ticks": int(fields[11]),
            "stime_ticks": int(fields[12]),
            "rss_bytes": int(fields[21]) * PAGE_SIZE,
        }
    except ValueError:
        return None


def _process_tree(root_pid: int) -> list[int]:
    by_parent: dict[int, list[int]] = {}
    try:
        proc_entries = list(Path("/proc").iterdir())
    except OSError:
        return [root_pid]
    for entry in proc_entries:
        if not entry.name.isdigit():
            continue
        text = _read_text(entry / "stat")
        if text is None:
            continue
        stat = _parse_proc_stat(text)
        if stat is None:
            continue
        by_parent.setdefault(stat["ppid"], []).append(int(entry.name))

    pids = [root_pid]
    for pid in pids:
        pids.extend(by_parent.get(pid, []))
    return pids


def _read_process_io(pid: int) -> dict[str, int]:
    result = {}
    text = _read_text(Path("/proc") / str(pid) / "io")
    if text is None:
        return result
    for line in text.splitlines():
        key, value = line.split(":", 1)
        if key in {"read_bytes", "write_bytes", "cancelled_write_bytes"}:
            try:
                result[key] = int(value.strip())
            except ValueError:
                pass
    return result


def _sample_process(root_pid: int) -> dict[str, Any]:
    aggregate = {
        "pid": root_pid,
        "pids": [],
        "cpu_user_s": 0.0,
        "cpu_system_s": 0.0,
        "rss_bytes": 0,
        "read_bytes": 0,
        "write_bytes": 0,
        "cancelled_write_bytes": 0,
    }

    for pid in _process_tree(root_pid):
        text = _read_text(Path("/proc") / str(pid) / "stat")
        if text is None:
            continue
        stat = _parse_proc_stat(text)
        if stat is None:
            continue
        io_stats = _read_process_io(pid)
        aggregate["pids"].append(pid)
        aggregate["cpu_user_s"] += stat["utime_ticks"] / CLK_TCK
        aggregate["cpu_system_s"] += stat["stime_ticks"] / CLK_TCK
        aggregate["rss_bytes"] += stat["rss_bytes"]
        aggregate["read_bytes"] += io_stats.get("read_bytes", 0)
        aggregate["write_bytes"] += io_stats.get("write_bytes", 0)
        aggregate["cancelled_write_bytes"] += io_stats.get(
            "cancelled_write_bytes", 0
        )

    return aggregate


def _sample_psi() -> dict[str, str]:
    result = {}
    for name in ("cpu", "memory", "io"):
        text = _read_text(Path("/proc/pressure") / name)
        if text is not None:
            result[name] = text
    return result


def sample_cpufreq() -> dict[str, Any]:
    cpus = {}
    for cpu in sorted(Path("/sys/devices/system/cpu").glob("cpu[0-9]*")):
        freq_dir = cpu / "cpufreq"
        if not freq_dir.exists():
            continue
        cpus[cpu.name] = {
            "scaling_cur_freq_khz": _read_int(freq_dir / "scaling_cur_freq"),
            "scaling_min_freq_khz": _read_int(freq_dir / "scaling_min_freq"),
            "scaling_max_freq_khz": _read_int(freq_dir / "scaling_max_freq"),
            "scaling_governor": _read_text(freq_dir / "scaling_governor"),
        }
    return cpus


def _mountinfo_for(path: Path) -> tuple[str, str] | None:
    existing = path
    while not existing.exists() and existing != existing.parent:
        existing = existing.parent
    try:
        target = existing.resolve()
    except OSError:
        target = existing

    best: tuple[str, str] | None = None
    best_len = -1
    text = _read_text(Path("/proc/self/mountinfo"))
    if text is None:
        return None
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 5:
            continue
        major_minor = fields[2]
        mount_point = fields[4].replace("\\040", " ")
        mount_path = Path(mount_point)
        try:
            target.relative_to(mount_path)
        except ValueError:
            continue
        if len(mount_point) > best_len:
            best = (major_minor, mount_point)
            best_len = len(mount_point)
    return best


def _device_name_for_major_minor(major_minor: str) -> str | None:
    dev_path = Path("/sys/dev/block") / major_minor
    try:
        block_path = dev_path.resolve()
    except OSError:
        return None
    block = block_path
    while block.parent.name != "block" and block != block.parent:
        block = block.parent
    return block.name


def datadir_disk(datadir: Path) -> dict[str, str] | None:
    mountinfo = _mountinfo_for(datadir)
    if mountinfo is None:
        return None
    major_minor, mount_point = mountinfo
    device = _device_name_for_major_minor(major_minor)
    return {
        "major_minor": major_minor,
        "mount_point": mount_point,
        "device": device or major_minor,
    }


def _sample_diskstats(device: str | None) -> dict[str, int] | None:
    if device is None:
        return None
    text = _read_text(Path("/proc/diskstats"))
    if text is None:
        return None
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 14 or fields[2] != device:
            continue
        values = {}
        for key, value in zip(DISKSTAT_FIELDS, fields[3:], strict=False):
            try:
                values[key] = int(value)
            except ValueError:
                pass
        return values
    return None


def _sample_netdev() -> dict[str, dict[str, int]]:
    text = _read_text(Path("/proc/net/dev"))
    if text is None:
        return {}
    result = {}
    for line in text.splitlines()[2:]:
        if ":" not in line:
            continue
        iface, data = line.split(":", 1)
        fields = data.split()
        if len(fields) < 16:
            continue
        try:
            result[iface.strip()] = {
                "rx_bytes": int(fields[0]),
                "rx_packets": int(fields[1]),
                "tx_bytes": int(fields[8]),
                "tx_packets": int(fields[9]),
            }
        except ValueError:
            pass
    return result


class Sampler:
    """Write telemetry samples until a child process exits."""

    def __init__(
        self,
        output_file: Path,
        datadir: Path,
        interval_s: float = 1.0,
        clock: Callable[[], float] = time.monotonic,
    ):
        self.output_file = output_file
        self.datadir = datadir
        self.interval_s = interval_s
        self.clock = clock
        self.disk = datadir_disk(datadir)

    def sample(self, pid: int, elapsed_s: float) -> dict[str, Any]:
        disk_device = self.disk["device"] if self.disk else None
        return {
            "timestamp": datetime.now(UTC).isoformat(),
            "elapsed_s": round(elapsed_s, 3),
            "process": _sample_process(pid),
            "host": {
                "psi": _sample_psi(),
                "cpufreq": sample_cpufreq(),
                "disk": self.disk,
                "diskstats": _sample_diskstats(disk_device),
                "netdev": _sample_netdev(),
            },
        }

    def run(self, process: subprocess.Popen[bytes]) -> int:
        self.output_file.parent.mkdir(parents=True, exist_ok=True)
        start = self.clock()
        with self.output_file.open("w") as f:
            while True:
                elapsed_s = self.clock() - start
                f.write(json.dumps(self.sample(process.pid, elapsed_s)) + "\n")
                f.flush()

                status = process.poll()
                if status is not None:
                    return status

                sleep_until = self.clock() + self.interval_s
                while self.clock() < sleep_until:
                    status = process.poll()
                    if status is not None:
                        return status
                    time.sleep(min(0.1, sleep_until - self.clock()))


def _metrics_file(output_dir: Path, name: str, run_index_file: Path) -> Path:
    run_index = _read_text(run_index_file) or "unknown"
    return output_dir / f"{name}-run-{run_index}-metrics.jsonl"


def _forward_signal(process: subprocess.Popen[bytes], signum: int) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signum)
    except OSError:
        try:
            process.send_signal(signum)
        except OSError:
            pass


def run_command(
    output_dir: Path,
    name: str,
    run_index_file: Path,
    datadir: Path,
    command: list[str],
) -> int:
    if not command:
        raise ValueError("command must not be empty")

    process = subprocess.Popen(command, start_new_session=True)
    previous_handlers = {
        signum: signal.getsignal(signum)
        for signum in (signal.SIGINT, signal.SIGTERM)
    }

    def handle_signal(signum: int, _frame: object) -> None:
        _forward_signal(process, signum)

    for signum in previous_handlers:
        signal.signal(signum, handle_signal)

    try:
        status = Sampler(
            output_file=_metrics_file(output_dir, name, run_index_file),
            datadir=datadir,
        ).run(process)
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)

    if status < 0:
        return 128 + abs(status)
    return status


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--run-index-file", required=True, type=Path)
    parser.add_argument("--datadir", required=True, type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)

    command = args.command
    if command[:1] == ["--"]:
        command = command[1:]
    return run_command(
        output_dir=args.output_dir,
        name=args.name,
        run_index_file=args.run_index_file,
        datadir=args.datadir,
        command=command,
    )


if __name__ == "__main__":
    sys.exit(main())
