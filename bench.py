#!/usr/bin/env python3
"""Benchcoin - Bitcoin Core benchmarking toolkit.

A unified CLI for building, benchmarking, analyzing, and reporting
on Bitcoin Core performance.

Usage:
    bench.py build BASE HEAD          Build bitcoind at two commits
    bench.py run BASE HEAD            Run benchmark
    bench.py analyze LOGFILE          Generate plots from debug.log
    bench.py report INPUT OUTPUT      Generate HTML report
    bench.py full BASE HEAD           Complete pipeline: build → run → analyze
"""

from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path

from bench.capabilities import detect_capabilities
from bench.config import build_config

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format="%(levelname)s: %(message)s",
)
logger = logging.getLogger(__name__)


def cmd_build(args: argparse.Namespace) -> int:
    """Build bitcoind at two commits."""
    from bench.build import BuildPhase

    capabilities = detect_capabilities()
    config = build_config(
        cli_args={
            "binaries_dir": args.binaries_dir,
            "skip_existing": args.skip_existing,
            "no_cpu_pinning": args.no_cpu_pinning,
            "dry_run": args.dry_run,
            "verbose": args.verbose,
        },
        config_file=Path(args.config) if args.config else None,
        profile=args.profile,
    )

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    phase = BuildPhase(config, capabilities)

    try:
        result = phase.run(
            args.base_commit,
            args.head_commit,
            binaries_dir=Path(args.binaries_dir) if args.binaries_dir else None,
        )
        logger.info(f"Built base binary: {result.base_binary}")
        logger.info(f"Built head binary: {result.head_binary}")
        return 0
    except Exception as e:
        logger.error(f"Build failed: {e}")
        return 1


def cmd_run(args: argparse.Namespace) -> int:
    """Run benchmark comparing two commits."""
    from bench.benchmark import BenchmarkPhase

    capabilities = detect_capabilities()
    config = build_config(
        cli_args={
            "datadir": args.datadir,
            "tmp_datadir": args.tmp_datadir,
            "binaries_dir": args.binaries_dir,
            "output_dir": args.output_dir,
            "stop_height": args.stop_height,
            "dbcache": args.dbcache,
            "runs": args.runs,
            "connect": args.connect,
            "chain": args.chain,
            "instrumented": args.instrumented,
            "no_cpu_pinning": args.no_cpu_pinning,
            "no_cache_drop": args.no_cache_drop,
            "dry_run": args.dry_run,
            "verbose": args.verbose,
        },
        config_file=Path(args.config) if args.config else None,
        profile=args.profile,
    )

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    # Validate config
    errors = config.validate()
    if errors:
        for error in errors:
            logger.error(error)
        return 1

    # Check binaries exist
    binaries_dir = (
        Path(args.binaries_dir) if args.binaries_dir else Path(config.binaries_dir)
    )
    base_binary = binaries_dir / "base" / "bitcoind"
    head_binary = binaries_dir / "head" / "bitcoind"

    if not base_binary.exists():
        logger.error(f"Base binary not found: {base_binary}")
        logger.error("Run 'bench.py build' first")
        return 1

    if not head_binary.exists():
        logger.error(f"Head binary not found: {head_binary}")
        logger.error("Run 'bench.py build' first")
        return 1

    phase = BenchmarkPhase(config, capabilities)

    try:
        result = phase.run(
            base_commit=args.base_commit,
            head_commit=args.head_commit,
            base_binary=base_binary,
            head_binary=head_binary,
            datadir=Path(config.datadir),
            output_dir=Path(config.output_dir),
        )
        logger.info(f"Results saved to: {result.results_file}")
        return 0
    except Exception as e:
        logger.error(f"Benchmark failed: {e}")
        if args.verbose:
            import traceback

            traceback.print_exc()
        return 1


def cmd_analyze(args: argparse.Namespace) -> int:
    """Generate plots from debug.log."""
    from bench.analyze import AnalyzePhase

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    log_file = Path(args.log_file)
    output_dir = Path(args.output_dir)

    if not log_file.exists():
        logger.error(f"Log file not found: {log_file}")
        return 1

    phase = AnalyzePhase()

    try:
        result = phase.run(
            commit=args.commit,
            log_file=log_file,
            output_dir=output_dir,
        )
        logger.info(f"Generated {len(result.plots)} plots in {result.output_dir}")
        return 0
    except Exception as e:
        logger.error(f"Analysis failed: {e}")
        if args.verbose:
            import traceback

            traceback.print_exc()
        return 1


def cmd_report(args: argparse.Namespace) -> int:
    """Generate HTML report from benchmark results."""
    from bench.report import ReportPhase

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    input_dir = Path(args.input_dir)
    output_dir = Path(args.output_dir)

    if not input_dir.exists():
        logger.error(f"Input directory not found: {input_dir}")
        return 1

    phase = ReportPhase()

    try:
        result = phase.run(
            input_dir=input_dir,
            output_dir=output_dir,
            title=args.title or "Benchmark Results",
        )

        # Print speedups
        if result.speedups:
            logger.info("Speedups:")
            for network, speedup in result.speedups.items():
                sign = "+" if speedup > 0 else ""
                logger.info(f"  {network}: {sign}{speedup}%")

        return 0
    except Exception as e:
        logger.error(f"Report generation failed: {e}")
        if args.verbose:
            import traceback

            traceback.print_exc()
        return 1


def cmd_full(args: argparse.Namespace) -> int:
    """Run full pipeline: build → run → analyze."""
    from bench.analyze import AnalyzePhase
    from bench.benchmark import BenchmarkPhase
    from bench.build import BuildPhase
    from bench.utils import find_debug_log

    capabilities = detect_capabilities()
    config = build_config(
        cli_args={
            "datadir": args.datadir,
            "tmp_datadir": args.tmp_datadir,
            "binaries_dir": args.binaries_dir,
            "output_dir": args.output_dir,
            "stop_height": args.stop_height,
            "dbcache": args.dbcache,
            "runs": args.runs,
            "connect": args.connect,
            "chain": args.chain,
            "instrumented": args.instrumented,
            "skip_existing": args.skip_existing,
            "no_cpu_pinning": args.no_cpu_pinning,
            "no_cache_drop": args.no_cache_drop,
            "dry_run": args.dry_run,
            "verbose": args.verbose,
        },
        config_file=Path(args.config) if args.config else None,
        profile=args.profile,
    )

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    # Validate config
    errors = config.validate()
    if errors:
        for error in errors:
            logger.error(error)
        return 1

    output_dir = Path(config.output_dir)
    binaries_dir = Path(config.binaries_dir)

    # Phase 1: Build
    logger.info("=== Phase 1: Build ===")
    build_phase = BuildPhase(config, capabilities)

    try:
        build_result = build_phase.run(
            args.base_commit,
            args.head_commit,
            binaries_dir=binaries_dir,
        )
    except Exception as e:
        logger.error(f"Build failed: {e}")
        return 1

    # Phase 2: Benchmark
    logger.info("=== Phase 2: Benchmark ===")
    benchmark_phase = BenchmarkPhase(config, capabilities)

    try:
        benchmark_result = benchmark_phase.run(
            base_commit=build_result.base_commit,
            head_commit=build_result.head_commit,
            base_binary=build_result.base_binary,
            head_binary=build_result.head_binary,
            datadir=Path(config.datadir),
            output_dir=output_dir,
        )
    except Exception as e:
        logger.error(f"Benchmark failed: {e}")
        return 1

    # Phase 3: Analyze (for instrumented runs)
    if config.instrumented:
        logger.info("=== Phase 3: Analyze ===")
        analyze_phase = AnalyzePhase(config)

        # Analyze base debug log
        if benchmark_result.debug_log_base:
            try:
                analyze_phase.run(
                    commit=build_result.base_commit,
                    log_file=benchmark_result.debug_log_base,
                    output_dir=output_dir / "plots",
                )
            except Exception as e:
                logger.warning(f"Analysis for base failed: {e}")

        # Analyze head debug log
        if benchmark_result.debug_log_head:
            try:
                analyze_phase.run(
                    commit=build_result.head_commit,
                    log_file=benchmark_result.debug_log_head,
                    output_dir=output_dir / "plots",
                )
            except Exception as e:
                logger.warning(f"Analysis for head failed: {e}")

    logger.info("=== Complete ===")
    logger.info(f"Results: {benchmark_result.results_file}")
    return 0


def main() -> int:
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="Benchcoin - Bitcoin Core benchmarking toolkit",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    # Global options
    parser.add_argument(
        "--config",
        metavar="PATH",
        help="Config file (default: bench.toml)",
    )
    parser.add_argument(
        "--profile",
        choices=["quick", "full", "ci"],
        default="full",
        help="Configuration profile (default: full)",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Verbose output",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show what would be done without executing",
    )

    subparsers = parser.add_subparsers(dest="command", help="Commands")

    # Build command
    build_parser = subparsers.add_parser("build", help="Build bitcoind at two commits")
    build_parser.add_argument("base_commit", help="Base commit (for comparison)")
    build_parser.add_argument("head_commit", help="Head commit (new code)")
    build_parser.add_argument(
        "--binaries-dir",
        metavar="PATH",
        help="Where to store binaries (default: ./binaries)",
    )
    build_parser.add_argument(
        "--skip-existing",
        action="store_true",
        help="Skip build if binary already exists",
    )
    build_parser.add_argument(
        "--no-cpu-pinning",
        action="store_true",
        help="Disable CPU affinity",
    )
    build_parser.set_defaults(func=cmd_build)

    # Run command
    run_parser = subparsers.add_parser("run", help="Run benchmark")
    run_parser.add_argument("base_commit", help="Base commit hash")
    run_parser.add_argument("head_commit", help="Head commit hash")
    run_parser.add_argument(
        "--datadir",
        required=True,
        metavar="PATH",
        help="Source datadir with blockchain snapshot",
    )
    run_parser.add_argument(
        "--tmp-datadir",
        metavar="PATH",
        help="Temp datadir for benchmark runs",
    )
    run_parser.add_argument(
        "--binaries-dir",
        metavar="PATH",
        help="Location of pre-built binaries",
    )
    run_parser.add_argument(
        "--output-dir",
        metavar="PATH",
        help="Output directory for results",
    )
    run_parser.add_argument(
        "--stop-height",
        type=int,
        metavar="N",
        help="Block height to stop at",
    )
    run_parser.add_argument(
        "--dbcache",
        type=int,
        metavar="N",
        help="Database cache size in MB",
    )
    run_parser.add_argument(
        "--runs",
        type=int,
        metavar="N",
        help="Number of benchmark iterations",
    )
    run_parser.add_argument(
        "--connect",
        metavar="ADDR",
        help="Connect address for sync",
    )
    run_parser.add_argument(
        "--chain",
        choices=["main", "testnet", "signet", "regtest"],
        help="Chain to use",
    )
    run_parser.add_argument(
        "--instrumented",
        action="store_true",
        help="Enable profiling (flamegraph + debug logging)",
    )
    run_parser.add_argument(
        "--no-cpu-pinning",
        action="store_true",
        help="Disable CPU affinity and scheduler priority",
    )
    run_parser.add_argument(
        "--no-cache-drop",
        action="store_true",
        help="Skip cache dropping between runs",
    )
    run_parser.set_defaults(func=cmd_run)

    # Analyze command
    analyze_parser = subparsers.add_parser(
        "analyze", help="Generate plots from debug.log"
    )
    analyze_parser.add_argument("commit", help="Commit hash (for naming)")
    analyze_parser.add_argument("log_file", help="Path to debug.log")
    analyze_parser.add_argument(
        "--output-dir",
        default="./plots",
        metavar="PATH",
        help="Output directory for plots",
    )
    analyze_parser.set_defaults(func=cmd_analyze)

    # Report command
    report_parser = subparsers.add_parser("report", help="Generate HTML report")
    report_parser.add_argument("input_dir", help="Directory with results.json")
    report_parser.add_argument("output_dir", help="Output directory for report")
    report_parser.add_argument(
        "--title",
        help="Report title",
    )
    report_parser.set_defaults(func=cmd_report)

    # Full command
    full_parser = subparsers.add_parser(
        "full", help="Full pipeline: build → run → analyze"
    )
    full_parser.add_argument("base_commit", help="Base commit (for comparison)")
    full_parser.add_argument("head_commit", help="Head commit (new code)")
    full_parser.add_argument(
        "--datadir",
        required=True,
        metavar="PATH",
        help="Source datadir with blockchain snapshot",
    )
    full_parser.add_argument(
        "--tmp-datadir",
        metavar="PATH",
        help="Temp datadir for benchmark runs",
    )
    full_parser.add_argument(
        "--binaries-dir",
        metavar="PATH",
        help="Where to store binaries",
    )
    full_parser.add_argument(
        "--output-dir",
        metavar="PATH",
        help="Output directory for results",
    )
    full_parser.add_argument(
        "--stop-height",
        type=int,
        metavar="N",
        help="Block height to stop at",
    )
    full_parser.add_argument(
        "--dbcache",
        type=int,
        metavar="N",
        help="Database cache size in MB",
    )
    full_parser.add_argument(
        "--runs",
        type=int,
        metavar="N",
        help="Number of benchmark iterations",
    )
    full_parser.add_argument(
        "--connect",
        metavar="ADDR",
        help="Connect address for sync",
    )
    full_parser.add_argument(
        "--chain",
        choices=["main", "testnet", "signet", "regtest"],
        help="Chain to use",
    )
    full_parser.add_argument(
        "--instrumented",
        action="store_true",
        help="Enable profiling (flamegraph + debug logging)",
    )
    full_parser.add_argument(
        "--skip-existing",
        action="store_true",
        help="Skip build if binary already exists",
    )
    full_parser.add_argument(
        "--no-cpu-pinning",
        action="store_true",
        help="Disable CPU affinity and scheduler priority",
    )
    full_parser.add_argument(
        "--no-cache-drop",
        action="store_true",
        help="Skip cache dropping between runs",
    )
    full_parser.set_defaults(func=cmd_full)

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        return 1

    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
