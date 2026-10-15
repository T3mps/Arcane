#!/usr/bin/env bash
# record-r2-responses.sh -- captures what aws-cli really prints against R2 into
# tests/fake-aws/responses/, which the fake aws replays (see the README there).
# Run once with the R2 token in the environment (plan Task 14), and again after
# an aws-cli bump. Every call works under a throwaway fixture-capture-<time>/
# prefix, which is deleted at the end; nothing else in the bucket is touched.
#   R2_ACCOUNT_ID R2_BUCKET AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../lib.sh"
r2_init
out="$here/fake-aws/responses"
p="fixture-capture-$(date -u +%Y%m%dT%H%M%SZ)/"
work=$(mktemp -d)
echo capture > "$work/body"
cleanup() {
    for k in a missing-never-written; do
        r2 delete-object --bucket "$R2_BUCKET" --key "${p}$k" >/dev/null 2>&1 || true
    done
}
trap cleanup EXIT

# capture NAME EXPECTED_EXIT CALL...: stdout to NAME.stdout.json on success,
# stderr to NAME.stderr on failure; any other exit status stops the run.
capture() {
    local name=$1 want=$2 rc=0
    shift 2
    r2 "$@" >"$work/stdout" 2>"$work/stderr" || rc=$?
    [ "$rc" = "$want" ] || { echo "$name: exit $rc, expected $want" >&2; cat "$work/stderr" >&2; exit 1; }
    if [ "$want" = 0 ]; then cp "$work/stdout" "$out/$name.stdout.json"; else cp "$work/stderr" "$out/$name.stderr"; fi
    echo "recorded $name"
}

capture put-object 0 put-object --bucket "$R2_BUCKET" --key "${p}a" --body "$work/body" --if-none-match '*' \
    --content-type text/plain --cache-control 'public, max-age=31536000, immutable' --metadata "sha256=$(sha256_of "$work/body")"
capture put-object-precondition-failed 254 put-object --bucket "$R2_BUCKET" --key "${p}a" --body "$work/body" --if-none-match '*'
# The If-Match failure must read the same as If-None-Match's; lib.sh treats both alike.
r2 put-object --bucket "$R2_BUCKET" --key "${p}a" --body "$work/body" --if-match '"00000000000000000000000000000000"' \
    >/dev/null 2>"$work/ifmatch" && { echo "an If-Match with a wrong ETag succeeded" >&2; exit 1; }
grep -q PreconditionFailed "$work/ifmatch" || { echo "If-Match failure is not PreconditionFailed:" >&2; cat "$work/ifmatch" >&2; exit 1; }
capture head-object 0 head-object --bucket "$R2_BUCKET" --key "${p}a"
capture get-object 0 get-object --bucket "$R2_BUCKET" --key "${p}a" "$work/got"
capture get-object-no-such-key 254 get-object --bucket "$R2_BUCKET" --key "${p}missing-never-written" "$work/none"
capture head-object-not-found 254 head-object --bucket "$R2_BUCKET" --key "${p}missing-never-written"
capture list-objects-v2 0 list-objects-v2 --bucket "$R2_BUCKET" --prefix "$p"
capture list-objects-v2-empty 0 list-objects-v2 --bucket "$R2_BUCKET" --prefix "${p}nothing-here/"
echo "done: review the diff under $out, drop the README's PROVISIONAL paragraph, run test-r2.sh, commit"
