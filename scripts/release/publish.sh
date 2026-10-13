#!/usr/bin/env bash
# publish.sh MODE PREFIX VERSION RELEASED_AT NOTES DIST WORK -- the publish job
# (spec s4, engine job steps 6-7): read and verify the current manifest, add
# the release and sign it locally, and only then upload the archives (never
# overwriting) and publish the manifest. A bad key, a missing or tampered
# manifest, or a bad dist folder therefore writes nothing. A version already
# published with these exact archives is a no-op, so a re-run that failed
# after the manifest write goes on to the steps after it. MODE publish goes to
# the bucket root with the release key; rehearsal goes to staging/ with the
# test key.
. "$(dirname "$0")/lib.sh"
[ $# -eq 7 ] || die "usage: publish.sh MODE PREFIX VERSION RELEASED_AT NOTES DIST WORK"
mode=$1 prefix=$2 version=$3 released_at=$4 notes=$5 dist=$6 work=$7
case $mode in
    publish) [ -z "$prefix" ] || die "a publish goes to the bucket root" ;;
    rehearsal|rehearsal-unsigned) [ "$prefix" = staging/ ] || die "a rehearsal goes to staging/" ;;
    *) die "unknown mode $mode" ;;
esac
r2_init
select_signing_key "$mode"
get_manifest "$work/current" "$prefix"
cur=(--manifest "$work/current/releases.json" --sig "$work/current/releases.json.minisig")
"$ARCRELEASE" verify --signature-only "${cur[@]}" "${TRUST[@]}" >/dev/null \
    || die "${prefix}releases.json does not verify, so nothing is built on it; run manifest-admin.yml with op=repair"
if jq -e --arg v "$version" 'any(.releases[]; .version == $v)' "$work/current/releases.json" >/dev/null; then
    published=$(jq -r --arg v "$version" '.releases[] | select(.version == $v) | .artifacts[] | "\(.path) \(.sha256)"' \
        "$work/current/releases.json" | tr -d '\r' | LC_ALL=C sort)
    staged=$(for f in "$dist"/*/arcane-"$version"-*; do
        if [ -f "$f" ]; then echo "engine/$version/$(basename "$f") $(sha256_of "$f")"; fi
    done | LC_ALL=C sort)
    [ "$published" = "$staged" ] || die "$version is already published with different archives; versions are immutable"
    echo "$version is already published, identical: nothing to upload"
    exit 0
fi
"$ARCRELEASE" add "${cur[@]}" "${TRUST[@]}" --min-serial "$(history_floor "$prefix")" \
    --dist "$dist" --version "$version" --released-at "$released_at" --notes-file "$notes" --out-dir "$work/next"
upload_dist "$dist" "$prefix" "$version"
publish_manifest "$work/next" "$prefix" "$(cat "$work/current/etag")"
