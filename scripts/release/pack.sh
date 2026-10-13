#!/usr/bin/env bash
# pack.sh STAGE OUTDIR VERSION OS -- the engine and symbols archives and a copy
# of engine.json, named as the publish step expects (dist::archive_name).
set -euo pipefail
stage=$1 out=$2 version=$3 os=$4
ext=tar.gz
[ "$os" = windows ] && ext=zip
mkdir -p "$out"
arcrelease pack --stage "$stage/engine" --root "arcane-$version" --out "$out/arcane-$version-$os-x86_64.$ext"
arcrelease pack --stage "$stage/symbols" --root "arcane-$version-symbols" --out "$out/arcane-$version-$os-x86_64-symbols.$ext"
cp "$stage/engine/engine.json" "$out/engine.json"
