#!/usr/bin/env bash
# check-pinned.sh FILE... -- every `uses:` that is not a local ./ action must
# be pinned to a 40-hex commit SHA (roadmap D26, spec s4). Exit 1 names each.
set -euo pipefail
bad=0
for f in "$@"; do
    while IFS= read -r ref; do
        case $ref in ./*) continue ;; esac
        [[ $ref =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@[0-9a-f]{40}$ ]] || { echo "$f: not pinned to a commit SHA: $ref" >&2; bad=1; }
    done < <(sed -n 's/^[[:space:]]*-\{0,1\}[[:space:]]*uses:[[:space:]]*\([^[:space:]#]*\).*/\1/p' "$f")
done
exit $bad
