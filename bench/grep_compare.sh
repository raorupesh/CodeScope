#!/usr/bin/env bash
# Baseline for the README: median `grep -r` time per query in queries.txt.
#   bench/grep_compare.sh <repo> [queries-file] [runs]
set -euo pipefail

repo=${1:?usage: grep_compare.sh <repo> [queries-file] [runs]}
queries=${2:-"$(dirname "$0")/queries.txt"}
runs=${3:-5}
[[ "$runs" =~ ^[1-9][0-9]*$ ]] || { echo "runs must be a positive integer" >&2; exit 2; }

excludes=(--exclude-dir=.git --exclude-dir=.hg --exclude-dir=.svn --exclude-dir=node_modules
          --exclude-dir=build --exclude-dir=third_party)

echo "| Query | grep -rFi median ms |"
echo "|---|---:|"
while IFS= read -r q || [[ -n "$q" ]]; do
  q=${q%$'\r'}
  [[ -z "$q" ]] && continue
  times=()
  for ((i = 0; i < runs; i++)); do
    start=$(date +%s%N)
    grep -rFiI "${excludes[@]}" -e "$q" "$repo" > /dev/null || true
    end=$(date +%s%N)
    times+=( $(( (end - start) / 1000000 )) )
  done
  median=$(printf '%s\n' "${times[@]}" | sort -n | sed -n "$(( (runs + 1) / 2 ))p")
  echo "| \`$q\` | $median |"
done < "$queries"
