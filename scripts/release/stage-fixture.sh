#!/usr/bin/env bash
# stage-fixture.sh VERSION OS STAGE -- a stand-in engine stage for dry runs and
# rehearsals until phase 1's stage-engine exists. The "engine" is the
# arcrelease binary (a real exe, so Windows signing is exercised for real),
# placed where the SDK keeps its binaries (Binaries/<Config>-<os>-<arch>-md/,
# the modularization layout), plus engine.json, the LICENSE, and a symbols
# folder.
set -euo pipefail
version=$1 os=$2 stage=$3
exe=arcrelease
[ "$os" = windows ] && exe=arcrelease.exe
bin=${ARCRELEASE_BIN:-$(command -v arcrelease)}
[ -f "$bin" ] || bin="$bin.exe"
[ -f "$bin" ] || { echo "stage-fixture: arcrelease is not installed" >&2; exit 1; }
dir="$stage/engine/Binaries/Release-$os-x86_64-md/arcrelease"
mkdir -p "$dir" "$stage/symbols"
cp "$bin" "$dir/$exe"
cp LICENSE "$stage/engine/LICENSE"
printf '{"version":"%s","build":"%s","abi":0,"channel":"preview"}\n' "$version" \
    "${GITHUB_SHA:-0000000000000000000000000000000000000000}" >"$stage/engine/engine.json"
echo "Fixture symbols for $version ($os): a rehearsal has no debug files." >"$stage/symbols/README.txt"
