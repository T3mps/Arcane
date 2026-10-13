#!/usr/bin/env bash
# dry-run-manifest.sh DIST VERSION RELEASED_AT NOTES -- the manifest a release
# would publish, built against an empty test manifest with the TEST key,
# verified, and written to the job summary. No network, no secrets.
. "$(dirname "$0")/lib.sh"
dist=$1 version=$2 released_at=$3 notes=$4
select_signing_key dry-run
work=$(mktemp -d)
"$ARCRELEASE" init "${TRUST[@]}" --out-dir "$work/m1" >/dev/null
"$ARCRELEASE" add --manifest "$work/m1/releases.json" --sig "$work/m1/releases.json.minisig" "${TRUST[@]}" \
    --dist "$dist" --version "$version" --released-at "$released_at" --notes-file "$notes" --out-dir "$work/m2" >/dev/null
"$ARCRELEASE" verify --manifest "$work/m2/releases.json" --sig "$work/m2/releases.json.minisig" "${TRUST[@]}"
{
    echo "### Dry run: the manifest entry $version would publish"
    echo '~~~json'
    cat "$work/m2/releases.json"
    echo '~~~'
} >>"${GITHUB_STEP_SUMMARY:-/dev/stdout}"
