#!/usr/bin/env bash

set -euxo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/prelude.sh"

# Executed after each timing run (no-op for uninstrumented)
conclude_run() {
  set -euxo pipefail
  return 0
}

run_benchmark() {
  local base_commit="$1"
  local head_commit="$2"
  local TMP_DATADIR="$3"
  local ORIGINAL_DATADIR="$4"
  local results_file="$5"
  local chain="$6"
  local stop_at_height="$7"
  local connect_address="$8"
  local dbcache="${9}"
  local BINARIES_DIR="${10}"

  # Export functions so they can be used by hyperfine
  export_shared_functions

  # Debug: Print all variables being used
  echo "=== Debug Information ==="
  echo "TMP_DATADIR: ${TMP_DATADIR}"
  echo "ORIGINAL_DATADIR: ${ORIGINAL_DATADIR}"
  echo "BINARIES_DIR: ${BINARIES_DIR}"
  echo "base_commit: ${base_commit}"
  echo "head_commit: ${head_commit}"
  echo "results_file: ${results_file}"
  echo "chain: ${chain}"
  echo "stop_at_height: ${stop_at_height}"
  echo "connect_address: ${connect_address}"
  echo "dbcache: ${dbcache}"
  printf '\n'

  # Run hyperfine
  hyperfine \
    --shell=bash \
    --setup "setup_run ${TMP_DATADIR}" \
    --prepare "prepare_run ${TMP_DATADIR} ${ORIGINAL_DATADIR}" \
    --cleanup "cleanup_run ${TMP_DATADIR}" \
    --runs 3 \
    --export-json "${results_file}" \
    --show-output \
    --command-name "base (${base_commit})" \
    --command-name "head (${head_commit})" \
    "taskset -c 2-15 chrt -o 0 ${BINARIES_DIR}/{commit}/bitcoind -datadir=${TMP_DATADIR} -connect=${connect_address} -daemon=0 -prune=10000 -chain=${chain} -stopatheight=${stop_at_height} -dbcache=${dbcache} -printtoconsole=0" \
    -L commit "base,head"
}

# Main execution
if [ "$#" -ne 10 ]; then
  echo "Usage: $0 base_commit head_commit TMP_DATADIR ORIGINAL_DATADIR results_dir chain stop_at_height connect_address dbcache BINARIES_DIR"
  exit 1
fi

run_benchmark "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8" "${9}" "${10}"
