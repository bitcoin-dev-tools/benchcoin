#!/usr/bin/env bash
# Shared functions

set -euxo pipefail

clean_datadir() {
  set -euxo pipefail

  local TMP_DATADIR="$1"

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

clean_logs() {
  set -euxo pipefail

  local TMP_DATADIR="$1"
  local logfile="${TMP_DATADIR}/debug.log"

  echo "Checking for ${logfile}"
  if [ -e "${logfile}" ]; then
    echo "Removing ${logfile}"
    rm "${logfile}"
  fi
}

# Executes once before each *set* of timing runs.
setup_run() {
  set -euxo pipefail

  local TMP_DATADIR="$1"
  clean_datadir "${TMP_DATADIR}"
}

# Executes before each timing run.
prepare_run() {
  set -euxo pipefail

  local TMP_DATADIR="$1"
  local ORIGINAL_DATADIR="$2"

  clean_datadir "${TMP_DATADIR}"
  # Don't copy hidden files so use *
  taskset -c 0-15 cp -r "$ORIGINAL_DATADIR"/* "$TMP_DATADIR"
  # Clear page caches
  sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
  clean_logs "${TMP_DATADIR}"
}

# Executes after the completion of all benchmarking runs for each individual
# command to be benchmarked.
cleanup_run() {
  set -euxo pipefail
  local TMP_DATADIR="$1"
  clean_datadir "${TMP_DATADIR}"
}

# Export all shared functions for use by hyperfine subshells
export_shared_functions() {
  export -f clean_datadir
  export -f clean_logs
  export -f setup_run
  export -f prepare_run
  export -f cleanup_run
}
