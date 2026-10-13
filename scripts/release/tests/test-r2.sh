#!/usr/bin/env bash
# Tests for scripts/release/{lib,publish,manifest-admin}.sh over a fake R2
# (tests/fake-aws) with the TEST manifest key. Needs bash, jq, md5sum,
# sha256sum and arcrelease on PATH:
#   cargo install --path ArcaneHub/crates/arcane_release --locked
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REL=$(cd "$HERE/.." && pwd)
ROOT=$(cd "$REL/../.." && pwd)
TEST_PUB="$ROOT/ArcaneHub/crates/arcane_release/tests/keys/test-manifest-1.pub"
command -v arcrelease >/dev/null || { echo "install arcrelease: cargo install --path ArcaneHub/crates/arcane_release --locked" >&2; exit 2; }
command -v jq >/dev/null || { echo "jq is required" >&2; exit 2; }

pass=0 fail=0
ok()   { pass=$((pass + 1)); echo "ok   $1"; }
bad()  { fail=$((fail + 1)); echo "FAIL $1"; }
expect()  { local name=$1; shift; if "$@" >"$T/out.log" 2>&1; then ok "$name"; else bad "$name"; sed 's/^/     /' "$T/out.log"; fi; }
refused() { local name=$1; shift; if "$@" >"$T/out.log" 2>&1; then bad "$name (was accepted)"; else ok "$name"; fi; }
said() { grep -q -- "$1" "$T/out.log"; }

setup() {
    T=$(mktemp -d)
    export FAKE_R2="$T/r2" PATH="$HERE/fake-aws:$PATH" R2_RETRY_DELAY=0
    export R2_ACCOUNT_ID=test R2_BUCKET=arcane-downloads AWS_ACCESS_KEY_ID=fake AWS_SECRET_ACCESS_KEY=fake
    mkdir -p "$FAKE_R2/objects"
    unset FAKE_AWS_FAIL_PUT
}
obj() { echo "$FAKE_R2/objects/$1"; }
serial_of() { jq -r .serial "$(obj "$1")"; }
pointer_verifies() {
    arcrelease verify --manifest "$(obj staging/releases.json)" --sig "$(obj staging/releases.json.minisig)" --trust-key "$TEST_PUB" >/dev/null 2>&1
}
make_dist() { # DIR VERSION
    local pair os ext
    for pair in windows:zip linux:tar.gz; do
        os=${pair%%:*} ext=${pair#*:}
        mkdir -p "$1/$os"
        printf '{"version":"%s","build":"0123456789abcdef0123456789abcdef01234567","abi":0,"channel":"preview"}' "$2" > "$1/$os/engine.json"
        echo "engine $os" > "$1/$os/arcane-$2-$os-x86_64.$ext"
        echo "symbols $os" > "$1/$os/arcane-$2-$os-x86_64-symbols.$ext"
    done
}
admin() { bash "$REL/manifest-admin.sh" "$@"; }
publish() { # VERSION
    bash "$REL/publish.sh" rehearsal staging/ "$1" 2026-10-12T00:00:00Z "$REL/fixtures/rehearsal-notes.md" "$T/dist" "$T/work-$RANDOM"
}

# --- put_immutable: once, identical re-upload fine, different content refused
setup
echo a > "$T/a"; echo b > "$T/b"
expect  "put_immutable uploads a new object"       bash -c ". '$REL/lib.sh'; r2_init; put_immutable '$T/a' x/a"
expect  "an identical re-upload is accepted"       bash -c ". '$REL/lib.sh'; r2_init; put_immutable '$T/a' x/a"
said "already uploaded, identical" && ok "  and says so" || bad "  and says so"
refused "different content under the same key"    bash -c ". '$REL/lib.sh'; r2_init; put_immutable '$T/b' x/a"
said "different content" && ok "  and says why" || bad "  and says why"
refused "a failed put of a missing object fails"   bash -c "export FAKE_AWS_FAIL_PUT=x/c; . '$REL/lib.sh'; r2_init; put_immutable '$T/a' x/c"

# --- init
setup
expect  "init staging"                             admin init staging
[ "$(serial_of staging/releases.json)" = 1 ] && ok "  serial 1" || bad "  serial 1"
[ -f "$(obj staging/history/000000001.json)" ] && ok "  history copy" || bad "  history copy"
pointer_verifies && ok "  pointer verifies" || bad "  pointer verifies"
refused "a second init is refused"                 admin init staging

# --- publish
setup; admin init staging >/dev/null; make_dist "$T/dist" 0.0.0-preview.1
expect  "publish a rehearsal"                      publish 0.0.0-preview.1
[ "$(find "$(obj staging/engine/0.0.0-preview.1)" -type f | wc -l)" -eq 4 ] && ok "  four archives" || bad "  four archives"
[ "$(serial_of staging/releases.json)" = 2 ] && ok "  serial 2" || bad "  serial 2"
pointer_verifies && ok "  pointer verifies" || bad "  pointer verifies"
refused "publishing the same version again"        publish 0.0.0-preview.1
said "already published" && ok "  says already published" || bad "  says already published"
[ "$(serial_of staging/releases.json)" = 2 ] && ok "  manifest unchanged" || bad "  manifest unchanged"

# --- rerun_after_partial: archives uploaded, manifest failed; a rerun completes
setup; admin init staging >/dev/null; make_dist "$T/dist" 0.0.0-preview.2
refused "a publish failing at the history write"   env FAKE_AWS_FAIL_PUT=staging/history/000000002.json bash "$REL/publish.sh" rehearsal staging/ 0.0.0-preview.2 2026-10-12T00:00:00Z "$REL/fixtures/rehearsal-notes.md" "$T/dist" "$T/w1"
[ "$(serial_of staging/releases.json)" = 1 ] && ok "  manifest untouched" || bad "  manifest untouched"
expect  "the rerun accepts the uploaded archives"  publish 0.0.0-preview.2
[ "$(serial_of staging/releases.json)" = 2 ] && ok "  and publishes serial 2" || bad "  and publishes serial 2"

# --- race: a manifest read before another run published is never written over
setup; admin init staging >/dev/null
bash -c ". '$REL/lib.sh'; r2_init; get_manifest '$T/stale' staging/" >/dev/null
expect  "a resign wins the race"                   admin resign staging
winner=$(md5sum "$(obj staging/releases.json)" | cut -d' ' -f1)
ARCANE_MINISIGN_KEY=$(cat "$ROOT/ArcaneHub/crates/arcane_release/tests/keys/test-manifest-1.key") ARCANE_MINISIGN_PASSWORD=arcane-test-key-not-a-secret \
    arcrelease resign --manifest "$T/stale/releases.json" --sig "$T/stale/releases.json.minisig" --trust-key "$TEST_PUB" --out-dir "$T/loser" >/dev/null
refused "the stale writer fails"                   bash -c ". '$REL/lib.sh'; r2_init; publish_manifest '$T/loser' staging/ \"\$(cat '$T/stale/etag')\""
[ "$(md5sum "$(obj staging/releases.json)" | cut -d' ' -f1)" = "$winner" ] && ok "  winner's manifest stands" || bad "  winner's manifest stands"
pointer_verifies && ok "  pointer verifies" || bad "  pointer verifies"

# --- half_update_and_repair: the json updated, the signature not
setup; admin init staging >/dev/null
refused "a resign whose signature upload fails"    env FAKE_AWS_FAIL_PUT=staging/releases.json.minisig bash "$REL/manifest-admin.sh" resign staging
said "op=repair" && ok "  names the repair" || bad "  names the repair"
pointer_verifies && bad "  pointer is half-updated" || ok "  pointer is half-updated"
refused "nothing builds on the half-updated pointer" admin resign staging
expect  "repair restores the newest verified pair" admin repair staging
pointer_verifies && ok "  pointer verifies" || bad "  pointer verifies"
[ "$(serial_of staging/releases.json)" = 2 ] && ok "  at serial 2" || bad "  at serial 2"

# --- tampered: refused, and the message says so
setup; admin init staging >/dev/null
sed -i 's/"serial": 1/"serial": 7/' "$(obj staging/releases.json)"
refused "a tampered pointer is refused"            admin resign staging
said "does not verify" && ok "  says it does not verify" || bad "  says it does not verify"

# --- yank
setup; admin init staging >/dev/null; make_dist "$T/dist" 0.0.0-preview.3; publish 0.0.0-preview.3 >/dev/null
expect  "yank a staging release"                   admin yank staging 0.0.0-preview.3 "Broken fixture."
[ "$(jq -r '.releases[0].yanked' "$(obj staging/releases.json)")" = true ] && ok "  yanked" || bad "  yanked"
refused "yank without a reason"                    admin yank staging 0.0.0-preview.3

# --- an empty dist uploads nothing
setup; admin init staging >/dev/null; mkdir -p "$T/dist"
refused "a publish with no archives"               publish 0.0.0-preview.4

echo "test-r2: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
