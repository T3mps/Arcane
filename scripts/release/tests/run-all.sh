#!/usr/bin/env bash
# Runs every scripts/release test (Linux, or WSL: needs jq and arcrelease).
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
status=0
for t in "$here"/test-*.sh; do
    echo "== $(basename "$t")"
    bash "$t" || status=1
done
exit $status
