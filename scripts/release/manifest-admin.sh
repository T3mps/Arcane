#!/usr/bin/env bash
# manifest-admin.sh OP TARGET [VERSION] [REASON] -- maintains releases.json
# (spec s4 "Re-signing", "Channels and yanking"; docs/releasing.md):
#   init    write serial 1 (the first publish only; refuses if one exists)
#   resign  the next serial with a fresh 90-day expiry (monthly)
#   yank    mark VERSION yanked with REASON
#   repair  restore releases.json + .minisig from the newest verified history copy
# TARGET production is the bucket root with the release key; staging is
# staging/ with the test key.
. "$(dirname "$0")/lib.sh"
op=${1:?usage: manifest-admin.sh OP TARGET [VERSION] [REASON]}
target=${2:?usage: manifest-admin.sh OP TARGET [VERSION] [REASON]}
version=${3:-} reason=${4:-}
case $target in
    production) prefix="" ;;
    staging) prefix="staging/" ;;
    *) die "unknown target $target" ;;
esac
r2_init
select_signing_key "$target"
work=$(mktemp -d)
case $op in
    init)
        if r2 head-object --bucket "$R2_BUCKET" --key "${prefix}releases.json" >/dev/null 2>&1; then
            die "${prefix}releases.json already exists; init is for the first publish only"
        fi
        "$ARCRELEASE" init "${TRUST[@]}" --min-serial "$(history_floor "$prefix")" --base-url "https://downloads.starworks.dev/${prefix}" --out-dir "$work/next"
        publish_manifest "$work/next" "$prefix" none ;;
    resign)
        get_manifest "$work/current" "$prefix"
        "$ARCRELEASE" resign --manifest "$work/current/releases.json" --sig "$work/current/releases.json.minisig" "${TRUST[@]}" \
            --min-serial "$(history_floor "$prefix")" --out-dir "$work/next"
        publish_manifest "$work/next" "$prefix" "$(cat "$work/current/etag")" ;;
    yank)
        [ -n "$version" ] && [ -n "$reason" ] || die "yank needs a version and a reason"
        get_manifest "$work/current" "$prefix"
        "$ARCRELEASE" yank --manifest "$work/current/releases.json" --sig "$work/current/releases.json.minisig" "${TRUST[@]}" \
            --version "$version" --reason "$reason" --min-serial "$(history_floor "$prefix")" --out-dir "$work/next"
        publish_manifest "$work/next" "$prefix" "$(cat "$work/current/etag")" ;;
    repair)
        repair_manifest "$prefix" ;;
    *) die "unknown op $op" ;;
esac
