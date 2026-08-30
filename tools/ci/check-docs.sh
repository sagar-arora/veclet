#!/usr/bin/env bash

set -euo pipefail

repository_root=$(git rev-parse --show-toplevel)
cd "$repository_root"

failure_count=0

while IFS= read -r -d '' document; do
  while IFS= read -r markdown_link; do
    target=${markdown_link#](}
    target=${target%)}

    case "$target" in
      http://*|https://*|mailto:*|'#'*)
        continue
        ;;
    esac

    target=${target%%#*}
    target=${target//%20/ }
    if [[ -z "$target" ]]; then
      continue
    fi

    if [[ "$target" == /* ]]; then
      resolved_path=".${target}"
    else
      resolved_path="$(dirname "$document")/${target}"
    fi

    if [[ ! -e "$resolved_path" ]]; then
      printf 'broken local link: %s -> %s\n' "$document" "$target" >&2
      failure_count=$((failure_count + 1))
    fi
  done < <(grep -oE '\]\([^)]+\)' "$document" || true)
done < <(find . -type f -name '*.md' -not -path './.git/*' -print0)

if ((failure_count > 0)); then
  printf 'documentation check failed with %d broken local link(s)\n' "$failure_count" >&2
  exit 1
fi

printf 'documentation links: ok\n'
