#!/usr/bin/env bash
# container-leg.sh <Config> <seed[,seed]> -- ONE local Linux leg inside the
# arcane-ci image, started by scripts/ci/local.ps1 (docs/ci.md, "Local runs").
#   /src   the desk checkout, read-only
#   /work  this leg's named volume (arcane-src-<leg>): incremental builds
#   /out   logs\local\<leg>-<Config>\ on the desk
# 1. mirror /src -> /work (rsync --delete, scripts/ci/rsync-filter.txt), then
#    CRLF -> LF for the files CI's Linux checkout has as LF (/out/lf-files.txt)
# 2. scripts/ci/build.sh <Config> -j $MAKE_JOBS
# 3. per seed: scripts/run-tests.sh under Xvfb with SDL's x11 driver, as CI
# 4. copy logs/* and test-results/ to /out; key=value results to /out/status.txt
#    (sync=, sync_seconds=, lf_converted=, build=, tests-<seed>=)
# Exit 0 when the build and every seed passed.
set -uo pipefail
CONFIG="${1:?usage: container-leg.sh <Config> <seed[,seed]>}"
SEEDS="${2:?usage: container-leg.sh <Config> <seed[,seed]>}"
OUT=/out
STATUS="$OUT/status.txt"
: > "$STATUS"
note() { echo "$1=$2" >> "$STATUS"; }

t0=$(date +%s)
if ! rsync -a --delete --filter="merge /src/scripts/ci/rsync-filter.txt" /src/ /work/; then
    echo "container-leg: rsync /src -> /work failed" >&2
    note sync 1
    exit 1
fi
note sync 0
note sync_seconds $(( $(date +%s) - t0 ))
echo "container-leg: synced /src -> /work in $(( $(date +%s) - t0 ))s"

# CI's Linux checkout has LF where a Windows (core.autocrlf) working tree has
# CRLF. local.ps1 lists those files (/out/lf-files.txt, from git on the desk);
# convert them in /work and keep the source's mtime, so make sees no change
# (rsync re-copies them each run -- the sizes differ -- and they convert again).
lf=0
if [ -f "$OUT/lf-files.txt" ]; then
    while IFS= read -r f; do
        if [ -n "$f" ] && [ -f "/work/$f" ] && grep -q "$(printf '\r')" "/work/$f"; then
            sed -i 's/\r$//' "/work/$f" && touch -r "/src/$f" "/work/$f" && lf=$((lf + 1))
        fi
    done < "$OUT/lf-files.txt"
fi
note lf_converted "$lf"
echo "container-leg: $lf CRLF file(s) converted to LF"

cd /work || exit 1
rm -rf logs test-results
scripts/ci/build.sh "$CONFIG" -j "${MAKE_JOBS:-2}"
build=$?
note build "$build"
# An out-of-memory VM kills cc1plus/ld, not the container: make exits 2. The
# kill shows up in the step logs; only then is the failure called OOM.
if [ "$build" -ne 0 ] && grep -qE "Killed signal terminated program|terminated with signal 9|signal 9 \(Killed\)" logs/build-*.log 2> /dev/null; then
    note oom 1
fi
failed=$build
if [ "$build" -eq 0 ]; then
    export SDL_VIDEODRIVER=x11
    for seed in $(echo "$SEEDS" | tr ',' ' '); do
        xvfb-run -a -s "-screen 0 1920x1080x24" scripts/run-tests.sh "$CONFIG" --rng-seed "$seed" \
            > "logs/tests-seed$seed.log" 2>&1
        rc=$?
        note "tests-$seed" "$rc"
        echo "container-leg: seed $seed -> exit $rc"
        tail -n 4 "logs/tests-seed$seed.log"
        [ "$rc" -eq 0 ] || failed=1
    done
fi
cp logs/* "$OUT/" 2> /dev/null || true
if [ -d test-results ]; then cp -R test-results "$OUT/"; fi
exit "$failed"
