#!/usr/bin/env bash
# publish.sh MODE PREFIX VERSION RELEASED_AT NOTES DIST WORK -- the publish job
# (spec s4, engine job steps 6-7): upload the archives (never overwriting),
# read the current manifest, add the release, sign, publish. MODE publish goes
# to the bucket root with the release key; rehearsal goes to staging/ with the
# test key.
. "$(dirname "$0")/lib.sh"
[ $# -eq 7 ] || die "usage: publish.sh MODE PREFIX VERSION RELEASED_AT NOTES DIST WORK"
mode=$1 prefix=$2 version=$3 released_at=$4 notes=$5 dist=$6 work=$7
case $mode in
    publish) [ -z "$prefix" ] || die "a publish goes to the bucket root" ;;
    rehearsal) [ "$prefix" = staging/ ] || die "a rehearsal goes to staging/" ;;
    *) die "unknown mode $mode" ;;
esac
r2_init
select_signing_key "$mode"
upload_dist "$dist" "$prefix" "$version"
get_manifest "$work/current" "$prefix"
"$ARCRELEASE" add --manifest "$work/current/releases.json" --sig "$work/current/releases.json.minisig" "${TRUST[@]}" \
    --dist "$dist" --version "$version" --released-at "$released_at" --notes-file "$notes" --out-dir "$work/next"
publish_manifest "$work/next" "$prefix" "$(cat "$work/current/etag")"
