#!/usr/bin/env bash
set -euxo pipefail

if [ $# -ne 2 ]; then
    echo "Usage: $0 <base_commit> <head_commit>"
    exit 1
fi

# Save current state of git
initial_ref=$(git symbolic-ref -q HEAD || git rev-parse HEAD)
if git symbolic-ref -q HEAD >/dev/null; then
    initial_state="branch"
    initial_branch=${initial_ref#refs/heads/}
else
    initial_state="detached"
fi

base_commit="$1"
head_commit="$2"

mkdir -p binaries/base
mkdir -p binaries/head

for build in "base:${base_commit}" "head:${head_commit}"; do
  name="${build%%:*}"
  commit="${build#*:}"
  git checkout "$commit"
  taskset -c 0-15 nix build -L
  cp "./result/bin/bitcoind" "./binaries/${name}/bitcoind"
  rm -rf "./result"
done

# Restore initial git state
if [ "$initial_state" = "branch" ]; then
    git checkout "$initial_branch"
else
    git checkout "$initial_ref"
fi
