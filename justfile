set shell := ["bash", "-uc"]

default:
    just --list

# ============================================================================
# Local benchmarking commands
# ============================================================================

# Test instrumented run using signet (includes report generation)
[group('local')]
test-instrumented base head datadir:
    nix develop --command python3 bench.py --profile quick full --chain signet --instrumented --skip-existing --datadir {{ datadir }} {{ base }} {{ head }}
    nix develop --command python3 bench.py report bench-output/ bench-output/

# Test uninstrumented run using signet
[group('local')]
test-uninstrumented base head datadir:
    nix develop --command python3 bench.py --profile quick full --chain signet --skip-existing --datadir {{ datadir }} {{ base }} {{ head }}

# Full benchmark with instrumentation (flamegraphs + plots)
[group('local')]
instrumented base head datadir:
    python3 bench.py --profile quick full --instrumented --datadir {{ datadir }} {{ base }} {{ head }}

# Just build binaries (useful for incremental testing)
[group('local')]
build base head:
    python3 bench.py build {{ base }} {{ head }}

# Run benchmark with pre-built binaries
[group('local')]
run base head datadir:
    python3 bench.py run --datadir {{ datadir }} {{ base }} {{ head }}

# Generate plots from a debug.log file
[group('local')]
analyze commit logfile output_dir="./plots":
    python3 bench.py analyze {{ commit }} {{ logfile }} --output-dir {{ output_dir }}

# Generate HTML report from benchmark results
[group('local')]
report input_dir output_dir:
    python3 bench.py report {{ input_dir }} {{ output_dir }}

# ============================================================================
# CI commands (called by GitHub Actions)
# ============================================================================

# Build binaries for CI
[group('ci')]
ci-build base_commit head_commit binaries_dir:
    python3 bench.py build --binaries-dir {{ binaries_dir }} {{ base_commit }} {{ head_commit }}

# Run uninstrumented benchmarks for CI
[group('ci')]
ci-run base_commit head_commit datadir tmp_datadir output_dir dbcache binaries_dir:
    python3 bench.py --profile ci run \
        --binaries-dir {{ binaries_dir }} \
        --datadir {{ datadir }} \
        --tmp-datadir {{ tmp_datadir }} \
        --output-dir {{ output_dir }} \
        --dbcache {{ dbcache }} \
        {{ base_commit }} {{ head_commit }}

# Run instrumented benchmarks for CI
[group('ci')]
ci-run-instrumented base_commit head_commit datadir tmp_datadir output_dir dbcache binaries_dir:
    python3 bench.py --profile ci run \
        --instrumented \
        --binaries-dir {{ binaries_dir }} \
        --datadir {{ datadir }} \
        --tmp-datadir {{ tmp_datadir }} \
        --output-dir {{ output_dir }} \
        --dbcache {{ dbcache }} \
        {{ base_commit }} {{ head_commit }}

# ============================================================================
# Git helpers
# ============================================================================

# Cherry-pick commits from a Bitcoin Core PR onto this branch
[group('git')]
pick-pr pr_number:
    #!/usr/bin/env bash
    set -euxo pipefail

    if ! git remote get-url upstream 2>/dev/null | grep -q "bitcoin/bitcoin"; then
        echo "Error: 'upstream' remote not found or doesn't point to bitcoin/bitcoin"
        echo "Please add it with: git remote add upstream https://github.com/bitcoin/bitcoin.git"
        exit 1
    fi

    git fetch upstream pull/{{ pr_number }}/head:bench-{{ pr_number }} && git cherry-pick $(git rev-list --reverse bench-{{ pr_number }} --not upstream/master)
