#!/usr/bin/env bash

set -euo pipefail

baseline_ref=${1:-main}
repository_root=$(git rev-parse --show-toplevel)
cd "$repository_root"

if ! baseline_commit=$(git rev-parse --verify "${baseline_ref}^{commit}"); then
  printf 'protobuf baseline is not a local commit: %s\n' "$baseline_ref" >&2
  exit 1
fi

baseline_files=$(git ls-tree -r --name-only "$baseline_commit" -- api/proto)
if ! grep -Eq '\.proto$' <<<"$baseline_files"; then
  printf 'protobuf breaking check: no prior schema; %s establishes the baseline\n' "$baseline_ref"
  exit 0
fi

buf breaking --against ".git#commit=${baseline_commit}"
printf 'protobuf breaking check against %s: ok\n' "$baseline_ref"
