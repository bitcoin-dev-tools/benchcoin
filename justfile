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
[private]
[group('ci')]
build-ci: clean
    cmake -B build -DBUILD_BENCH=ON
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

# Run the CI workflow
[group('ci')]
run-ci: build-ci bench test
