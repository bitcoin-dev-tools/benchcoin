#!/usr/bin/env python3
"""Benchcoin - Bitcoin Core benchmarking toolkit.

A CLI for building, benchmarking, analyzing, and reporting on Bitcoin Core
performance.

Usage:
    bench.py build COMMIT[:NAME]...   Build bitcoind at one or more commits
    bench.py run NAME:BINARY...       Benchmark one or more binaries
    bench.py analyze COMMIT LOGFILE   Generate plots from debug.log
    bench.py compare RESULTS...       Compare benchmark results
    bench.py report INPUT OUTPUT      Generate HTML report

Examples:
    # Build two commits
    bench.py build HEAD~1:before HEAD:after

    # Benchmark built binaries
    bench.py run before:./binaries/before/bitcoind after:./binaries/after/bitcoind --datadir /data

    # Compare results
    bench.py compare ./bench-output/results.json

    # Generate HTML report
    bench.py report ./bench-output ./report
"""

from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path

from bench.capabilities import detect_capabilities
from bench.config import build_config

logging.basicConfig(
    level=logging.INFO,
    format="%(levelname)s: %(message)s",
)
logger = logging.getLogger(__name__)


def cmd_build(args: argparse.Namespace) -> int:
    """Build bitcoind at one or more commits."""
    from bench.build import BuildPhase

    capabilities = detect_capabilities()
    config = build_config(
        cli_args={
            "binaries_dir": args.output_dir,
            "skip_existing": args.skip_existing,
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
            args.commits,
            output_dir=Path(args.output_dir) if args.output_dir else None,
        )
        logger.info(f"Built {len(result.binaries)} binary(ies):")
        for binary in result.binaries:
            logger.info(f"  {binary.name}: {binary.path}")
        return 0
    except Exception as e:
        logger.error(f"Build failed: {e}")
        return 1


def cmd_run(args: argparse.Namespace) -> int:
    """Run benchmark on one or more binaries."""
    from bench.benchmark import BenchmarkPhase, parse_binary_spec

    capabilities = detect_capabilities()
    config = build_config(
        cli_args={
            "datadir": args.datadir,
            "tmp_datadir": args.tmp_datadir,
            "output_dir": args.output_dir,
            "stop_height": args.stop_height,
            "dbcache": args.dbcache,
            "runs": args.runs,
            "connect": args.connect,
            "chain": args.chain,
            "instrumented": args.instrumented,
            "no_cache_drop": args.no_cache_drop,
            "dry_run": args.dry_run,
            "verbose": args.verbose,
        },
        config_file=Path(args.config) if args.config else None,
        profile=args.profile,
    )

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    errors = config.validate()
    if errors:
        for error in errors:
            logger.error(error)
        return 1

    # Parse binary specs
    try:
        binaries = [parse_binary_spec(spec) for spec in args.binaries]
    except ValueError as e:
        logger.error(str(e))
        return 1

    # Validate binaries exist
    for name, path in binaries:
        if not path.exists():
            logger.error(f"Binary not found: {path} ({name})")
            return 1

    phase = BenchmarkPhase(config, capabilities)
    output_dir = Path(config.output_dir)

    try:
        result = phase.run(
            binaries=binaries,
            datadir=Path(config.datadir),
            output_dir=output_dir,
        )
        logger.info(f"Results saved to: {result.results_file}")

        # For instrumented runs, also generate plots
        if config.instrumented:
            from bench.analyze import AnalyzePhase

            analyze_phase = AnalyzePhase()

            for binary_result in result.binaries:
                if binary_result.debug_log:
                    try:
                        analyze_phase.run(
                            commit=binary_result.name,
                            log_file=binary_result.debug_log,
                            output_dir=output_dir / "plots",
                        )
                    except Exception as e:
                        logger.warning(f"Analysis for {binary_result.name} failed: {e}")

        return 0
    except Exception as e:
        logger.error(f"Benchmark failed: {e}")
        if args.verbose:
            import traceback

            traceback.print_exc()
        return 1


def cmd_compare(args: argparse.Namespace) -> int:
    """Compare benchmark results from multiple files."""
    from bench.compare import ComparePhase

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    results_files = [Path(f) for f in args.results_files]

    # Validate files exist
    for f in results_files:
        if not f.exists():
            logger.error(f"Results file not found: {f}")
            return 1

    phase = ComparePhase()

    try:
        result = phase.run(results_files, baseline=args.baseline)

        # Output results
        output_json = phase.to_json(result)

        if args.output:
            output_path = Path(args.output)
            output_path.write_text(output_json)
            logger.info(f"Comparison saved to: {output_path}")
        else:
            print(output_json)

        return 0
    except Exception as e:
        logger.error(f"Comparison failed: {e}")
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


def main() -> int:
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="Benchcoin - Bitcoin Core benchmarking toolkit",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

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
    build_parser = subparsers.add_parser(
        "build",
        help="Build bitcoind at one or more commits",
        description="Build bitcoind binaries from git commits. "
        "Each commit can optionally have a name suffix: COMMIT:NAME",
    )
    build_parser.add_argument(
        "commits",
        nargs="+",
        metavar="COMMIT[:NAME]",
        help="Commit(s) to build. Format: COMMIT or COMMIT:NAME (e.g., HEAD:latest, abc123:v27)",
    )
    build_parser.add_argument(
        "-o",
        "--output-dir",
        metavar="PATH",
        help="Where to store binaries (default: ./binaries)",
    )
    build_parser.add_argument(
        "--skip-existing",
        action="store_true",
        help="Skip build if binary already exists",
    )
    build_parser.set_defaults(func=cmd_build)

    # Run command
    run_parser = subparsers.add_parser(
        "run",
        help="Run benchmark on one or more binaries",
        description="Benchmark bitcoind binaries using hyperfine. "
        "Each binary must have a name and path: NAME:PATH",
    )
    run_parser.add_argument(
        "binaries",
        nargs="+",
        metavar="NAME:PATH",
        help="Binary(ies) to benchmark. Format: NAME:PATH (e.g., v27:./binaries/v27/bitcoind)",
    )
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
        "-o",
        "--output-dir",
        metavar="PATH",
        help="Output directory for results (default: ./bench-output)",
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

    # Compare command
    compare_parser = subparsers.add_parser(
        "compare",
        help="Compare benchmark results from multiple files",
        description="Load and compare results from one or more results.json files. "
        "Calculates speedup percentages relative to a baseline.",
    )
    compare_parser.add_argument(
        "results_files",
        nargs="+",
        metavar="RESULTS_FILE",
        help="results.json file(s) to compare",
    )
    compare_parser.add_argument(
        "--baseline",
        metavar="NAME",
        help="Name of the baseline entry (default: first entry)",
    )
    compare_parser.add_argument(
        "-o",
        "--output",
        metavar="FILE",
        help="Output file for comparison JSON (default: stdout)",
    )
    compare_parser.set_defaults(func=cmd_compare)

    # Report command
    report_parser = subparsers.add_parser("report", help="Generate HTML report")
    report_parser.add_argument("input_dir", help="Directory with results.json")
    report_parser.add_argument("output_dir", help="Output directory for report")
    report_parser.add_argument(
        "--title",
        help="Report title",
    )
    report_parser.set_defaults(func=cmd_report)

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        return 1

    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
