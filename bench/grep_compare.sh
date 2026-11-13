#!/usr/bin/env bash
# Baseline for the README: time `grep -r` on the same queries codescope_bench uses.
#
#   bench/grep_compare.sh <repo> [queries-file] [runs]
#
# Each query runs `runs` times (default 5); the median wall time is reported.
# grep scans every file on every query, which is what the trigram index avoids.
set -euo pipefail

repo=${1:?usage: grep_compare.sh <repo> [queries-file] [runs]}
queries=${2:-"$(dirname "$0")/queries.txt"}
runs=${3:-5}

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
