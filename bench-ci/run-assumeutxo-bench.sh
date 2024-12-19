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

# Helper function to clear logs
clean_logs() {
  set -euxo pipefail

  local TMP_DATADIR="$1"
  local logfile="${TMP_DATADIR}/debug.log"

  echo "Checking for ${logfile}"
  if [ -e "{$logfile}" ]; then
    echo "Removing ${logfile}"
    rm "${logfile}"
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

  local TMP_DATADIR="$1"
  local UTXO_PATH="$2"
  local CONNECT_ADDRESS="$3"
  local chain="$4"

  # Run the actual preparation steps
  clean_datadir "${TMP_DATADIR}"
  build/src/bitcoind -datadir="${TMP_DATADIR}" -connect="${CONNECT_ADDRESS}" -daemon=0 -chain="${chain}" -stopatheight=1
  build/src/bitcoind -datadir="${TMP_DATADIR}" -connect="${CONNECT_ADDRESS}" -daemon=0 -chain="${chain}" -dbcache=16000 -pausebackgroundsync=1 -loadutxosnapshot="${UTXO_PATH}" || true
  clean_logs "${TMP_DATADIR}"
}

# Executed after each timing run
conclude_assumeutxo_snapshot_run() {
  set -euxo pipefail

  local commit="$1"

  if [ -e flamegraph.html ]; then
    mv flamegraph.html "${commit}"-flamegraph.html
  fi
}

# Execute CMD after the completion of all benchmarking runs for each individual
# command to be benchmarked.
cleanup_assumeutxo_snapshot_run() {
  set -euxo pipefail

  local TMP_DATADIR="$1"

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
  export -f conclude_assumeutxo_snapshot_run
  export -f cleanup_assumeutxo_snapshot_run
  export -f clean_datadir
  export -f clean_logs

  # Run hyperfine
  hyperfine \
    --setup "taskset -c 1-15 setup_assumeutxo_snapshot_run {commit} ${TMP_DATADIR}" \
    --prepare "taskset -c 1-15 prepare_assumeutxo_snapshot_run ${TMP_DATADIR} ${UTXO_PATH} ${connect_address} ${chain}" \
    --conclude "conclude_assumeutxo_snapshot_run {commit}" \
    --cleanup "cleanup_assumeutxo_snapshot_run ${TMP_DATADIR}" \
    --runs 1 \
    --show-output \
    --export-json "${results_file}" \
    --command-name "base (${base_commit})" \
    --command-name "head (${head_commit})" \
    "taskset -c 1 perf script flamegraph taskset -c 2-15 build/src/bitcoind -datadir=${TMP_DATADIR} -connect=${connect_address} -daemon=0 -chain=${chain} -stopatheight=${stop_at_height}" \
    -L commit "${base_commit},${head_commit}"
}

# Main execution
if [ "$#" -ne 8 ]; then
  echo "Usage: $0 base_commit head_commit TMP_DATADIR UTXO_PATH results_dir chain stop_at_height connect_address"
  exit 1
fi

run_benchmark "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8"
