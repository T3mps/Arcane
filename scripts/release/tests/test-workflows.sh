#!/usr/bin/env bash
# Static checks on release.yml and manifest-admin.yml for rules a lint can't
# see (final review of the phase 2 plan): no workflow-level concurrency group
# (a newly queued run in a group cancels the one already pending, so a dry run
# could silently cancel a release waiting behind an approval; the conditional
# R2 writes are the guard), a dry run's manifest job that survives the skipped
# signing job, R2 credentials only in the steps that use them, and a publish
# whose GitHub Release and website steps also work on a re-run.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
REL_WF="$ROOT/.github/workflows/release.yml"
ADMIN_WF="$ROOT/.github/workflows/manifest-admin.yml"
pass=0 fail=0
check() { if eval "$2"; then pass=$((pass + 1)); echo "ok   $1"; else fail=$((fail + 1)); echo "FAIL $1"; fi; }

# job_block FILE JOB: the lines of one job (from "  JOB:" to the next job).
job_block() { awk -v job="  $2:" '$0 == job {on = 1; print; next} on && /^  [A-Za-z0-9_-]+:$/ {exit} on {print}' "$1"; }

for wf in "$REL_WF" "$ADMIN_WF"; do
    name=$(basename "$wf")
    check "$name: no workflow-level concurrency" "! grep -q '^concurrency:' '$wf'"
    check "$name: R2 credentials are not job-level env" "! grep -Eq '^      (AWS_ACCESS_KEY_ID|AWS_SECRET_ACCESS_KEY|R2_ACCOUNT_ID|R2_BUCKET):' '$wf'"
    check "$name: R2 credentials are still passed to a step" "grep -Eq '^          AWS_SECRET_ACCESS_KEY: \\\$\\{\\{ secrets.R2_SECRET_ACCESS_KEY \\}\\}' '$wf'"
done

dry=$(job_block "$REL_WF" dry-run-manifest)
check "dry-run-manifest runs although sign-windows was skipped" \
    "grep -q '!cancelled()' <<<\"\$dry\" && grep -q \"needs.package.result == 'success'\" <<<\"\$dry\""
check "the unsigned rehearsal is a dispatch option" "grep -q 'options: \[dry-run, rehearsal, rehearsal-unsigned\]' '$REL_WF'"
sign=$(job_block "$REL_WF" sign-windows)
check "sign-windows is skipped for an unsigned rehearsal" "grep -q \"mode != 'rehearsal-unsigned'\" <<<\"\$sign\""
pkg=$(job_block "$REL_WF" package)
check "unsigned Windows binaries are packed only for a dry run or an unsigned rehearsal" \
    "grep -q \"needs.prepare.outputs.mode == 'dry-run' || needs.prepare.outputs.mode == 'rehearsal-unsigned'\" <<<\"\$pkg\" && ! grep -q \"mode == 'publish'\" <<<\"\$pkg\""
pub=$(job_block "$REL_WF" publish)
check "publish needs a successful package job" "grep -q \"needs.package.result == 'success'\" <<<\"\$pub\""
check "the GitHub Release step tolerates an existing release" "grep -q 'gh release view' <<<\"\$pub\""
check "the website token step never fails the publish" "grep -B2 -A6 'create-github-app-token' <<<\"\$pub\" | grep -q 'continue-on-error: true'"

echo "test-workflows: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
