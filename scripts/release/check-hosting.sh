#!/usr/bin/env bash
# check-hosting.sh [BASE] -- checks downloads.starworks.dev serves the bucket as
# spec s3/s12 need: the manifest pair never edge-cached, archives cached.
# BASE defaults to the staging area; run it after a rehearsal.
set -euo pipefail
base=${1:-https://downloads.starworks.dev/staging/}
status() { curl -fsS -o /dev/null -D - "$1" | tr -d '\r' | awk -F': ' 'tolower($1) == "cf-cache-status" { print toupper($2) }'; }
fail=0
for f in releases.json releases.json.minisig; do
    curl -fsS -o /dev/null "$base$f"
    s=$(status "$base$f")
    if [ "$s" = HIT ]; then echo "FAIL $f is edge-cached; the bypass Cache Rule is missing"; fail=1; else echo "ok   $f (cf-cache-status ${s:-none})"; fi
done
archive=$(curl -fsS "${base}releases.json" | jq -r '.releases[0].artifacts[0].path // empty')
[ -n "$archive" ] || { echo "FAIL no release in ${base}releases.json (run a rehearsal first)"; exit 1; }
curl -fsS -o /dev/null "$base$archive"
s=$(status "$base$archive")
if [ "$s" = HIT ]; then echo "ok   $archive cached (HIT)"; else echo "FAIL $archive not cached on a second request (${s:-none})"; fail=1; fi
exit $fail
