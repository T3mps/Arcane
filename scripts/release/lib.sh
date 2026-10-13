# shellcheck shell=bash
# Shared by the release scripts (spec s4, s12): R2 through aws-cli's S3 API,
# never Wrangler. Archives are written once (If-None-Match: *); the manifest
# pointer only over the exact version that was read (If-Match: <etag>); every
# manifest serial is also kept immutably under history/. Secrets come from the
# environment and are never echoed.
set -euo pipefail

RELEASE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
TEST_KEY_DIR="$RELEASE_ROOT/ArcaneHub/crates/arcane_release/tests/keys"
ARCRELEASE=${ARCRELEASE:-arcrelease}
TRUST=()

die() { echo "release: $*" >&2; exit 1; }
need_env() { local v; for v in "$@"; do [ -n "${!v:-}" ] || die "$v is not set"; done; }

r2_init() {
    need_env R2_ACCOUNT_ID R2_BUCKET AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY
    export AWS_DEFAULT_REGION=auto
    # The scripts parse aws-cli's JSON: never a profile's text or table output, never a pager.
    export AWS_DEFAULT_OUTPUT=json AWS_PAGER=""
    # aws-cli 2.23+ computes request checksums R2 does not fully support (spec s12).
    export AWS_REQUEST_CHECKSUM_CALCULATION=WHEN_REQUIRED
    export AWS_RESPONSE_CHECKSUM_VALIDATION=WHEN_REQUIRED
    R2_ENDPOINT="https://${R2_ACCOUNT_ID}.r2.cloudflarestorage.com"
}

r2() { aws s3api --endpoint-url "$R2_ENDPOINT" "$@"; }

sha256_of() { sha256sum "$1" | cut -d' ' -f1; }

content_type_for() {
    case $1 in
        *.zip) echo application/zip ;;
        *.tar.gz) echo application/gzip ;;
        *.json) echo application/json ;;
        *.minisig|*.txt|*.md) echo text/plain ;;
        *) echo application/octet-stream ;;
    esac
}

# aws_error FILE: aws-cli's error text from a captured stderr, on one line.
aws_error() { tr -s '\r\n' '  ' <"$1" | sed 's/^ *//; s/ *$//'; }

# put_immutable FILE KEY [WHY]: upload once. If KEY exists already, the same
# SHA-256 counts as uploaded (a rerun); different content fails with WHY
# (default: versions are immutable).
put_immutable() {
    local file=$1 key=$2 sha existing err
    err=$(mktemp)
    sha=$(sha256_of "$file")
    if r2 put-object --bucket "$R2_BUCKET" --key "$key" --body "$file" --if-none-match '*' \
        --content-type "$(content_type_for "$key")" --cache-control 'public, max-age=31536000, immutable' \
        --metadata "sha256=$sha" >/dev/null 2>"$err"; then
        echo "uploaded $key"
        return 0
    fi
    existing=$(r2 head-object --bucket "$R2_BUCKET" --key "$key" 2>/dev/null | jq -r '.Metadata.sha256 // empty') || existing=""
    [ -n "$existing" ] || die "uploading $key failed: $(aws_error "$err")"
    [ "$existing" = "$sha" ] || die "${3:-$key already exists with different content (sha256 $existing); versions are immutable}"
    echo "already uploaded, identical: $key"
}

# upload_dist DIST PREFIX VERSION: every archive under DIST/<os>/ to PREFIXengine/VERSION/.
upload_dist() {
    local dist=$1 prefix=$2 version=$3 f n=0
    for f in "$dist"/*/arcane-"$version"-*; do
        [ -f "$f" ] || continue
        put_immutable "$f" "${prefix}engine/${version}/$(basename "$f")"
        n=$((n + 1))
    done
    [ "$n" -gt 0 ] || die "no archives for $version under $dist"
}

# get_manifest OUT PREFIX: the current pointer pair, read through the S3 API
# (never the CDN, which could be stale), and its ETag for the If-Match write.
get_manifest() {
    local out=$1 prefix=$2
    mkdir -p "$out"
    r2 get-object --bucket "$R2_BUCKET" --key "${prefix}releases.json" "$out/releases.json" >"$out/get.json" 2>"$out/get.err" \
        || die "cannot read ${prefix}releases.json ($(tr -d '\n' <"$out/get.err")); a first publish needs manifest-admin.yml op=init"
    jq -r .ETag "$out/get.json" >"$out/etag"
    r2 get-object --bucket "$R2_BUCKET" --key "${prefix}releases.json.minisig" "$out/releases.json.minisig" >/dev/null 2>&1 \
        || die "cannot read ${prefix}releases.json.minisig; run manifest-admin.yml op=repair"
}

# put_pointer DIR PREFIX ETAG|none: releases.json only over the version read
# (or only if absent), then its signature (retried). The pair cannot be
# written atomically; history/ is what repair restores from.
put_pointer() {
    local dir=$1 prefix=$2 etag=$3 i cond err
    err=$(mktemp)
    if [ "$etag" = none ]; then cond=(--if-none-match '*'); else cond=(--if-match "$etag"); fi
    r2 put-object --bucket "$R2_BUCKET" --key "${prefix}releases.json" --body "$dir/releases.json" "${cond[@]}" \
        --content-type application/json --cache-control no-cache >/dev/null 2>"$err" \
        || die "${prefix}releases.json was not written: $(aws_error "$err"). On PreconditionFailed another run published first and nothing was overwritten: re-run this workflow. On any other error the write's outcome is unknown: run manifest-admin.yml with op=repair, then re-run."
    for i in 1 2 3; do
        if r2 put-object --bucket "$R2_BUCKET" --key "${prefix}releases.json.minisig" --body "$dir/releases.json.minisig" \
            --content-type text/plain --cache-control no-cache >/dev/null 2>&1; then
            echo "published ${prefix}releases.json serial $(jq -r .serial "$dir/releases.json")"
            return 0
        fi
        [ "$i" -lt 3 ] && sleep "${R2_RETRY_DELAY:-5}"
    done
    die "${prefix}releases.json was updated but its signature was not; run manifest-admin.yml with op=repair (docs/releasing.md, \"Repair\")"
}

# history_floor PREFIX: 1 + the highest serial under PREFIXhistory/ (1 if none),
# passed to arcrelease as --min-serial. A run that died between its two history
# writes leaves its serial taken; the next manifest skips it (Hubs only need
# serials to rise), so one failed upload never blocks every later write.
history_floor() {
    local prefix=$1 listing top
    listing=$(r2 list-objects-v2 --bucket "$R2_BUCKET" --prefix "${prefix}history/") \
        || die "cannot list ${prefix}history/"
    top=$(jq -r '.Contents[]?.Key' <<<"$listing" | sed -n 's|.*/\([0-9]\{9\}\)\.json$|\1|p' | sort | tail -1)
    echo $((10#${top:-0} + 1))
}

# publish_manifest DIR PREFIX ETAG|none: the history copy FIRST, write-once, so
# repair always has a signed copy; then the pointer. The serial is above every
# history entry (history_floor), so a collision means another run published
# that serial first: this run's manifest is stale and nothing is overwritten.
publish_manifest() {
    local dir=$1 prefix=$2 etag=$3 serial hist race
    serial=$(jq -r .serial "$dir/releases.json")
    hist=$(printf '%shistory/%09d' "$prefix" "$serial")
    race="another run published serial $serial first; nothing was overwritten. Re-run this workflow."
    put_immutable "$dir/releases.json" "$hist.json" "$race"
    put_immutable "$dir/releases.json.minisig" "$hist.json.minisig" "$race"
    put_pointer "$dir" "$prefix" "$etag"
}

# repair_manifest PREFIX: restore the pointer pair from the newest history pair
# whose signature verifies.
repair_manifest() {
    local prefix=$1 work keys k found="" etag
    work=$(mktemp -d)
    keys=$(r2 list-objects-v2 --bucket "$R2_BUCKET" --prefix "${prefix}history/" | jq -r '.Contents[]?.Key' | grep '\.json$' | sort -r) || keys=""
    [ -n "$keys" ] || die "no history under ${prefix}history/"
    for k in $keys; do
        r2 get-object --bucket "$R2_BUCKET" --key "$k" "$work/releases.json" >/dev/null 2>&1 || continue
        r2 get-object --bucket "$R2_BUCKET" --key "$k.minisig" "$work/releases.json.minisig" >/dev/null 2>&1 || continue
        if "$ARCRELEASE" verify --signature-only --manifest "$work/releases.json" --sig "$work/releases.json.minisig" "${TRUST[@]}" >/dev/null 2>&1; then
            found=$k
            break
        fi
    done
    [ -n "$found" ] || die "no history pair under ${prefix}history/ verifies"
    etag=$(r2 head-object --bucket "$R2_BUCKET" --key "${prefix}releases.json" 2>/dev/null | jq -r .ETag) || etag=none
    put_pointer "$work" "$prefix" "$etag"
    echo "repaired ${prefix}releases.json from $found"
}

# select_signing_key MODE: production uses the release key from the
# environment and the keys built into arcrelease; staging, rehearsal and dry
# runs use the committed TEST key, which no Hub trusts.
select_signing_key() {
    case $1 in
        publish|production)
            need_env MINISIGN_SECRET_KEY MINISIGN_PASSWORD
            ARCANE_MINISIGN_KEY=$MINISIGN_SECRET_KEY
            ARCANE_MINISIGN_PASSWORD=$MINISIGN_PASSWORD
            TRUST=() ;;
        rehearsal|staging|dry-run)
            ARCANE_MINISIGN_KEY=$(cat "$TEST_KEY_DIR/test-manifest-1.key")
            ARCANE_MINISIGN_PASSWORD=arcane-test-key-not-a-secret
            TRUST=(--trust-key "$TEST_KEY_DIR/test-manifest-1.pub") ;;
        *) die "unknown signing mode $1" ;;
    esac
    export ARCANE_MINISIGN_KEY ARCANE_MINISIGN_PASSWORD
}
