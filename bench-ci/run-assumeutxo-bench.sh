#!/usr/bin/env bash

set -euxo pipefail

# Helper function to check and clean datadir
clean_datadir() {
  set -euxo pipefail

  local TMP_DATADIR="$1"

  # Create the directory if it doesn't exist
  mkdir -p "${TMP_DATADIR}"

  # If we're in CI, clean without confirmation
  if [ -n "${CI:-}" ]; then
    rm -Rf "${TMP_DATADIR:?}"/*
  else
    read -rp "Are you sure you want to delete everything in ${TMP_DATADIR}? [y/N] " response
    if [[ "$response" =~ ^[Yy]$ ]]; then
      rm -Rf "${TMP_DATADIR:?}"/*
    else
      echo "Aborting..."
      exit 1
    fi
  fi
}

# Execute CMD before each set of timing runs.
setup_assumeutxo_snapshot_run() {
  set -euxo pipefail

  local commit="$1"
  local TMP_DATADIR="$2"

  git checkout "${commit}"
  # Build for CI without bench_bitcoin
  cmake -B build -DBUILD_BENCH=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer"
  cmake --build build -j "$(nproc)"
  clean_datadir "${TMP_DATADIR}"
}

# Execute CMD before each timing run.
prepare_assumeutxo_snapshot_run() {
  set -euxo pipefail

  local commit="$1"
  local TMP_DATADIR="$2"
  local UTXO_PATH="$3"
  local CONNECT_ADDRESS="$4"
  local chain="$5"

  # Handle last_commit tracking and flamegraph movement
  if [ -e last_commit.txt ]; then
    LAST_COMMIT=$(cat last_commit.txt)
    if [ -e flamegraph.html ]; then
      mv flamegraph.html "${LAST_COMMIT}"-flamegraph.html
    fi
  fi

  # Store current commit
  echo "${commit}" >last_commit.txt

  # Run the actual preparation steps
  clean_datadir "${TMP_DATADIR}"
  build/src/bitcoind -datadir="${TMP_DATADIR}" -connect="${CONNECT_ADDRESS}" -daemon=0 -chain="${chain}" -stopatheight=1
  # TODO: remove the or true here. It's a hack as we currently get unclean exit
  build/src/bitcoind -datadir="${TMP_DATADIR}" -connect="${CONNECT_ADDRESS}" -daemon=0 -chain="${chain}" -dbcache=16000 -pausebackgroundsync=1 -loadutxosnapshot="${UTXO_PATH}" || true
}

# Execute CMD after the completion of all benchmarking runs for each individual
# command to be benchmarked.
cleanup_assumeutxo_snapshot_run() {
  set -euxo pipefail

  local TMP_DATADIR="$1"

  # Move current flamegraph if it exists
  if [ -e flamegraph.html ]; then
    CURRENT_COMMIT=$(cat last_commit.txt)
    mv flamegraph.html "${CURRENT_COMMIT}"-flamegraph.html
  fi

  # Clean up the datadir
  clean_datadir "${TMP_DATADIR}"
}

run_benchmark() {
  local base_commit="$1"
  local head_commit="$2"
  local TMP_DATADIR="$3"
  local UTXO_PATH="$4"
  local results_file="$5"
  local chain="$6"
  local stop_at_height="$7"
  local connect_address="$8"

  # Export functions so they can be used by hyperfine
  export -f setup_assumeutxo_snapshot_run
  export -f prepare_assumeutxo_snapshot_run
  export -f cleanup_assumeutxo_snapshot_run
  export -f clean_datadir

  # Run hyperfine
  hyperfine \
    --setup "setup_assumeutxo_snapshot_run {commit} ${TMP_DATADIR}" \
    --prepare "prepare_assumeutxo_snapshot_run {commit} ${TMP_DATADIR} ${UTXO_PATH} ${connect_address} ${chain}" \
    --cleanup "cleanup_assumeutxo_snapshot_run ${TMP_DATADIR}" \
    --runs 1 \
    --show-output \
    --export-json "${results_file}" \
    --command-name "base (${base_commit})" \
    --command-name "head (${head_commit})" \
    "perf script flamegraph build/src/bitcoind -datadir=${TMP_DATADIR} -connect=${connect_address} -daemon=0 -chain=${chain} -assumevalid=0 -stopatheight=${stop_at_height}" \
    -L commit "${base_commit},${head_commit}"
}

# Main execution
if [ "$#" -ne 8 ]; then
  echo "Usage: $0 base_commit head_commit TMP_DATADIR UTXO_PATH results_dir chain stop_at_height connect_address"
  exit 1
fi

run_benchmark "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8"
