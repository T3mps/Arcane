#!/usr/bin/env bash
# check-stage.sh STAGE VERSION OS -- before packing: engine.json names this
# version, and the staged binary still runs after signing (spec s4 step 4).
# Phase 1 replaces the fixture run with --print-engine-info compared against
# engine.json (version, commit, engineAbi).
set -euo pipefail
stage=$1 version=$2 os=$3
die() { echo "check-stage: $*" >&2; exit 1; }
jq -e --arg v "$version" '.version == $v' "$stage/engine/engine.json" >/dev/null || die "$stage/engine/engine.json is not $version"
exe="$stage/engine/Binaries/Release-$os-x86_64-md/arcrelease/arcrelease"
[ "$os" = windows ] && exe="$exe.exe"
[ -f "$exe" ] || die "no staged binary at $exe (phase 1: the engine's probe goes here)"
"$exe" --version
