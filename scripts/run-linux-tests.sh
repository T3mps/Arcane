#!/usr/bin/env bash
# Runs ArcaneTests on Linux the way the Linux CI lane does (.github/workflows/
# linux.yml): FROM the exe directory (data paths are exe-relative), headless
# (SDL_VIDEODRIVER=offscreen), with "~[gpu]" plus every case listed in
# scripts/linux-test-exclusions.txt excluded.
#
#   scripts/run-linux-tests.sh [--gpu] <config> [extra Catch2 args...]
#     config: Debug | Release | Dist   (bin/<config>-linux-x86_64-md/ArcaneTests)
#     --gpu:  run "[gpu]" instead of "~[gpu]" (same exclusions). Needs a Vulkan
#             ICD -- CI uses Mesa lavapipe -- and, for the windowed cases, a
#             display (xvfb-run + SDL_VIDEODRIVER=x11). Software adapters compare
#             against their own golden set, ReferenceProject/Verify/References/
#             vulkan-lavapipe/ (ReferenceImages.hpp, ReferenceAdapterSet).
#
# Exclusion file format: one case per line, "<exact test case name> | <reason>".
# Blank lines and lines starting with '#' are ignored. The reason is
# mandatory -- an entry without one fails this script, so the list cannot
# silently grow unexplained.
#
# Catch2 ANDs separate test-spec arguments, so each exclusion is passed as its
# own ~"name" argument after "~[gpu]". Inside a quoted name Catch2 still
# treats ',' as an OR separator and '\' as an escape, so both (and '"') are
# backslash-escaped here.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
GPU_SPEC="~[gpu]"
if [ "${1:-}" = "--gpu" ]; then
    GPU_SPEC="[gpu]"
    shift
fi
CONFIG="${1:-Debug}"
shift || true
EXE_DIR="$ROOT/bin/$CONFIG-linux-x86_64-md/ArcaneTests"
EXCLUSIONS="${ARCANE_TEST_EXCLUSIONS:-$ROOT/scripts/linux-test-exclusions.txt}"

if [ ! -x "$EXE_DIR/ArcaneTests" ]; then
    echo "run-linux-tests: $EXE_DIR/ArcaneTests not built" >&2
    exit 2
fi

specs=("$GPU_SPEC")
if [ -f "$EXCLUSIONS" ]; then
    lineno=0
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno + 1))
        case "$line" in ''|'#'*) continue ;; esac
        name="${line%% | *}"
        reason="${line#* | }"
        if [ "$name" = "$line" ] || [ -z "${reason// /}" ]; then
            echo "run-linux-tests: $EXCLUSIONS:$lineno has no ' | reason': $line" >&2
            exit 2
        fi
        name="${name%"${name##*[![:space:]]}"}"   # trim trailing blanks
        esc=${name//\\/\\\\}
        esc=${esc//,/\\,}
        esc=${esc//\"/\\\"}
        specs+=("~\"$esc\"")
    done < "$EXCLUSIONS"
fi

echo "run-linux-tests: $CONFIG, ${#specs[@]} spec(s) ($GPU_SPEC + $(( ${#specs[@]} - 1 )) exclusion(s))"
cd "$EXE_DIR"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}"
exec ./ArcaneTests "${specs[@]}" "$@"
