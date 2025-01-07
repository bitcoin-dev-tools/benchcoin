set shell := ["bash", "-uc"]

os := os()

default:
    just --list

# Build base and head binaries for CI
[group('ci')]
build-assumeutxo-binaries base_commit head_commit:
    #!/usr/bin/env bash
    set -euxo pipefail
    for build in "base:{{ base_commit }}" "head:{{ head_commit }}"; do
        name="${build%%:*}"
        commit="${build#*:}"
        git checkout "$commit"
        taskset -c 0-15 cmake -B "build-$name" \
            -DBUILD_BENCH=OFF \
            -DBUILD_CLI=OFF \
            -DBUILD_TESTS=OFF \
            -DBUILD_TX=OFF \
            -DBUILD_UTIL=OFF \
            -DENABLE_EXTERNAL_SIGNER=OFF \
            -DENABLE_WALLET=OFF \
            -DINSTALL_MAN=OFF \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer"
        taskset -c 0-15 cmake --build "build-$name" -j {{ num_cpus() }}
    done

# Run signet assumeutxo CI workflow
[group('ci')]
run-assumeutxo-signet-ci base_commit head_commit TMP_DATADIR UTXO_PATH results_file dbcache png_dir binaries_dir:
    ./bench-ci/run-assumeutxo-bench.sh {{ base_commit }} {{ head_commit }} {{ TMP_DATADIR }} {{ UTXO_PATH }} {{ results_file }} {{ png_dir }} signet 200000 "148.251.128.115:55555" {{ dbcache }} {{ binaries_dir }}

# Run mainnet assumeutxo CI workflow for default cache
[group('ci')]
run-assumeutxo-mainnet-default-ci base_commit head_commit TMP_DATADIR UTXO_PATH results_file dbcache png_dir binaries_dir:
    ./bench-ci/run-assumeutxo-bench.sh {{ base_commit }} {{ head_commit }} {{ TMP_DATADIR }} {{ UTXO_PATH }} {{ results_file }} {{ png_dir }} main 850000 "148.251.128.115:33333" {{ dbcache }} {{ binaries_dir }}

# Run mainnet assumeutxo CI workflow for large cache
[group('ci')]
run-assumeutxo-mainnet-large-ci base_commit head_commit TMP_DATADIR UTXO_PATH results_file dbcache png_dir binaries_dir:
    ./bench-ci/run-assumeutxo-bench.sh {{ base_commit }} {{ head_commit }} {{ TMP_DATADIR }} {{ UTXO_PATH }} {{ results_file }} {{ png_dir }} main 850000 "148.251.128.115:33333" {{ dbcache }} {{ binaries_dir }}

# Run a signet benchmark locally
[group('local')]
run-signet:
    #!/usr/bin/env bash
    set -euo pipefail
    set -x

    # Get git HEAD and merge-base with master (as BASE)
    HEAD=$(git rev-parse HEAD)
    BASE=$(git merge-base HEAD master)
    echo "Using BASE: $BASE"
    echo "Using HEAD: $HEAD"

    # Make a random temp dir and save it as TMPDIR
    TMPDIR=$(mktemp -d)
    echo "Using temporary directory: $TMPDIR"

    # Create required directories
    mkdir -p "$TMPDIR/datadir"
    mkdir -p "$TMPDIR/png"
    mkdir -p "$TMPDIR/binaries"

    # Build binaries
    just build-assumeutxo-binaries "$BASE" "$HEAD"
    cp build-head/src/bitcoind "$TMPDIR/binaries/bitcoind-head"
    cp build-base/src/bitcoind "$TMPDIR/binaries/bitcoind-base"

    # Fetch utxo-signet-160000.dat if not exists in $CWD
    if [ ! -f "./utxo-signet-160000.dat" ]; then
        echo "Downloading utxo-signet-160000.dat..."
        if command -v curl &> /dev/null; then
            curl -L -o "./utxo-signet-160000.dat" "https://tmp.256k1.dev/utxo-signet-160000.dat"
        elif command -v wget &> /dev/null; then
            wget -O "./utxo-signet-160000.dat" "https://tmp.256k1.dev/utxo-signet-160000.dat"
        else
            echo "Error: Neither curl nor wget is available. Please install one of them."
            exit 1
        fi
        echo "Download complete."
    else
        echo "Using existing utxo-signet-160000.dat"
    fi

    # Run signet CI
    CI=1 just run-assumeutxo-signet-ci \
        "$BASE" \
        "$HEAD" \
        "$TMPDIR/datadir" \
        "$PWD/utxo-signet-160000.dat" \
        "$TMPDIR/result" \
        16000 \
        "$TMPDIR/png" \
        "$TMPDIR/binaries"

    echo "Results saved in: $TMPDIR/result"
    echo "PNG files saved in: $TMPDIR/png"
