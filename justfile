set shell := ["bash", "-uc"]

os := os()

default:
    just --list

# Build default project
[group('build')]
build *args: clean
    cmake -B build {{ args }}
    cmake --build build -j {{ num_cpus() }}

# Build with all optional modules
[group('build')]
build-dev *args: clean
    cmake -B build --preset dev-mode {{ args }}
    cmake --build build -j {{ num_cpus() }}

# Build for the CI, including bench_bitcoin
[group('ci')]
[private]
build-ci: clean
    cmake -B build -DBUILD_BENCH=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAPPEND_CPPFLAGS="-fno-omit-frame-pointer"
    cmake --build build -j {{ num_cpus() }}

# Re-build current config
[group('build')]
rebuild:
    cmake --build build -j {{ num_cpus() }}

# Clean build dir using git clean -dfx
[group('build')]
clean:
    git clean -dfx

# Run unit tests
[group('test')]
test-unit:
    ctest --test-dir build -j {{ num_cpus() }}

# Run all functional tests
[group('test')]
test-func:
    build/test/functional/test_runner.py -j {{ num_cpus() }}

# Run all unit and functional tests
[group('test')]
test: test-unit test-func

# Run a single functional test (filename.py)
[group('test')]
test-func1 test:
    build/test/functional/test_runner.py {{ test }}

# Run a single unit test suite
[group('test')]
test-unit1 suite:
    build/src/test/test_bitcoin --log_level=all --run_test={{ suite }}

# Run benchmarks
[group('perf')]
bench:
    build/src/bench/bench_bitcoin

# Run the lint job
lint:
    #!/usr/bin/env bash
    cd test/lint/test_runner/
    cargo fmt
    cargo clippy
    export COMMIT_RANGE="$( git rev-list --max-count=1 --merges HEAD )..HEAD"
    RUST_BACKTRACE=1 cargo run

# Run signet assumeutxo CI workflow
[group('ci')]
run-assumeutxo-signet-ci base_commit head_commit TMP_DATADIR UTXO_PATH results_file dbcache png_dir:
    ./bench-ci/run-assumeutxo-bench.sh {{ base_commit }} {{ head_commit }} {{ TMP_DATADIR }} {{ UTXO_PATH }} {{ results_file }} {{ png_dir }} signet 200000 "148.251.128.115:55555" {{ dbcache }}

# Run mainnet assumeutxo CI workflow for default cache
[group('ci')]
run-assumeutxo-mainnet-default-ci base_commit head_commit TMP_DATADIR UTXO_PATH results_file dbcache png_dir:
    ./bench-ci/run-assumeutxo-bench.sh {{ base_commit }} {{ head_commit }} {{ TMP_DATADIR }} {{ UTXO_PATH }} {{ results_file }} {{ png_dir }} main 850000 "148.251.128.115:33333" {{ dbcache }}

# Run mainnet assumeutxo CI workflow for large cache
[group('ci')]
run-assumeutxo-mainnet-large-ci base_commit head_commit TMP_DATADIR UTXO_PATH results_file dbcache png_dir:
    ./bench-ci/run-assumeutxo-bench.sh {{ base_commit }} {{ head_commit }} {{ TMP_DATADIR }} {{ UTXO_PATH }} {{ results_file }} {{ png_dir }} main 850000 "148.251.128.115:33333" {{ dbcache }}
