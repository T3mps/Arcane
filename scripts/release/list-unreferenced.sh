#!/usr/bin/env bash
# list-unreferenced.sh PREFIX VERSION -- after a failed publish: the archives
# uploaded for VERSION that the current manifest doesn't reference (spec s9:
# they stay, unreferenced, and are listed). Prints Markdown for the job summary.
. "$(dirname "$0")/lib.sh"
prefix=$1 version=$2
r2_init
work=$(mktemp -d)
listed=""
if r2 get-object --bucket "$R2_BUCKET" --key "${prefix}releases.json" "$work/r.json" >/dev/null 2>&1; then
    listed=$(jq -r --arg v "$version" '.releases[] | select(.version == $v) | .artifacts[].path' "$work/r.json")
fi
echo "### Archives uploaded for $version that releases.json does not reference"
r2 list-objects-v2 --bucket "$R2_BUCKET" --prefix "${prefix}engine/${version}/" | jq -r '.Contents[]?.Key' | while read -r key; do
    grep -qxF "${key#"$prefix"}" <<<"$listed" || echo "- \`$key\`"
done
