#!/usr/bin/env bash
# Tests for check-pinned.sh and pin-actions.sh (roadmap D26: third-party
# actions pinned to commit SHAs).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REL=$(cd "$HERE/.." && pwd)
T=$(mktemp -d)
pass=0 fail=0
accepts() { local name=$1; shift; if "$@" >"$T/log" 2>&1; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); echo "FAIL $name"; cat "$T/log"; fi; }
refuses() { local name=$1; shift; if "$@" >"$T/log" 2>&1; then fail=$((fail + 1)); echo "FAIL $name (accepted)"; else pass=$((pass + 1)); echo "ok   $name"; fi; }
SHA=0123456789abcdef0123456789abcdef01234567

cat >"$T/wf.yml" <<EOF
jobs:
  a:
    steps:
      - uses: actions/checkout@v4
      - uses: ./.github/actions/arcrelease
      - uses: azure/login@$SHA # v3
      - name: x
        uses: dtolnay/rust-toolchain@stable
EOF
refuses "unpinned uses are reported"     bash "$REL/check-pinned.sh" "$T/wf.yml"
grep -q "actions/checkout@v4" "$T/log" && grep -q "dtolnay/rust-toolchain@stable" "$T/log" && pass=$((pass + 1)) && echo "ok     and named" || { fail=$((fail + 1)); echo "FAIL   and named"; }

mkdir -p "$T/bin"
printf '#!/bin/sh\necho %s\n' "$SHA" >"$T/bin/gh"
chmod +x "$T/bin/gh"
accepts "pin-actions pins them"          env PATH="$T/bin:$PATH" bash "$REL/pin-actions.sh" "$T/wf.yml"
accepts "and the file then passes"       bash "$REL/check-pinned.sh" "$T/wf.yml"
grep -q "uses: actions/checkout@$SHA # v4" "$T/wf.yml" && pass=$((pass + 1)) && echo "ok     keeps the tag as a comment" || { fail=$((fail + 1)); echo "FAIL   keeps the tag as a comment"; }
grep -q "uses: ./.github/actions/arcrelease" "$T/wf.yml" && pass=$((pass + 1)) && echo "ok     leaves local actions alone" || { fail=$((fail + 1)); echo "FAIL   leaves local actions alone"; }

echo "test-pin: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
