#!/usr/bin/env bash

set -euo pipefail

repository_root=$(git rev-parse --show-toplevel)
cd "$repository_root"

required_files=(
  AGENTS.md
  LICENSE
  README.md
  api/proto/AGENTS.md
  control/AGENTS.md
  engine/AGENTS.md
  deploy/AGENTS.md
  ingest/AGENTS.md
  tests/AGENTS.md
)

for required_file in "${required_files[@]}"; do
  if [[ ! -f "$required_file" ]]; then
    printf 'missing required repository file: %s\n' "$required_file" >&2
    exit 1
  fi
done

if trailing_whitespace=$(git grep -nI -E '[[:blank:]]+$' -- ':!LICENSE'); then
  printf '%s\n' "$trailing_whitespace" >&2
  printf 'repository check failed: tracked text contains trailing whitespace\n' >&2
  exit 1
fi

artifact_count=0
while IFS= read -r -d '' tracked_file; do
  case "$tracked_file" in
    build/*|*/build/*|cmake-build-*/*|*/cmake-build-*/*|out/*|*/out/*|bin/*|*/bin/*|\
    *.o|*.a|*.so|*.dylib|*.dll|*.exe|*.profraw|*.profdata)
      printf 'tracked generated artifact: %s\n' "$tracked_file" >&2
      artifact_count=$((artifact_count + 1))
      ;;
  esac
done < <(git ls-files -z)

if ((artifact_count > 0)); then
  printf 'repository check failed with %d tracked artifact(s)\n' "$artifact_count" >&2
  exit 1
fi

printf 'repository hygiene: ok\n'
