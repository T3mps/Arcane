#!/usr/bin/env bash
# The dry-run path end to end, locally: a fixture stage for both OSes, checked,
# packed, and turned into a verified manifest with the test key. Nothing is
# uploaded. Needs arcrelease on PATH.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REL=$(cd "$HERE/.." && pwd)
ROOT=$(cd "$REL/../.." && pwd)
T=$(mktemp -d)
V=0.0.0-preview.99
cd "$ROOT"
for os in windows linux; do
    bash "$REL/stage-fixture.sh" "$V" "$os" "$T/stage-$os"
    bash "$REL/check-stage.sh" "$T/stage-$os" "$V" "$os" >/dev/null
    bash "$REL/pack.sh" "$T/stage-$os" "$T/dist/$os" "$V" "$os" >/dev/null
done
[ -f "$T/dist/windows/arcane-$V-windows-x86_64.zip" ] || { echo "FAIL no windows zip"; exit 1; }
[ -f "$T/dist/linux/arcane-$V-linux-x86_64-symbols.tar.gz" ] || { echo "FAIL no linux symbols"; exit 1; }
# The engine archives use the SDK's layout (Binaries/<Config>-<os>-<arch>-md/,
# the modularization rename of bin/), under one root folder.
win_bin="arcane-$V/Binaries/Release-windows-x86_64-md/arcrelease/arcrelease.exe"
lin_bin="arcane-$V/Binaries/Release-linux-x86_64-md/arcrelease/arcrelease"
grep -qxF "$win_bin" <<<"$(unzip -Z1 "$T/dist/windows/arcane-$V-windows-x86_64.zip")" \
    || { echo "FAIL the Windows archive lacks $win_bin"; exit 1; }
# Listed first, then searched: `tar | grep -q` would fail under pipefail when
# grep exits early and tar hits the closed pipe.
listing=$(tar -tvzf "$T/dist/linux/arcane-$V-linux-x86_64.tar.gz")
# (Off Linux, Git Bash's cp names the copied exe arcrelease.exe; the exact name
# and the exec bit are checked on Linux, where CI runs this.)
grep -q " $lin_bin" <<<"$listing" || { echo "FAIL the Linux archive lacks $lin_bin"; echo "$listing"; exit 1; }
if [ "$(uname -s)" = Linux ]; then
    # ...named exactly, and keeping the binary's exec bit (tar mode 0755).
    grep -q "^-rwxr-xr-x .* $lin_bin$" <<<"$listing" \
        || { echo "FAIL the Linux archive lost the binary's exec bit"; echo "$listing"; exit 1; }
fi
GITHUB_STEP_SUMMARY="$T/summary.md" bash "$REL/dry-run-manifest.sh" "$T/dist" "$V" 2026-10-12T00:00:00Z "$REL/fixtures/rehearsal-notes.md"
grep -q "\"version\": \"$V\"" "$T/summary.md" || { echo "FAIL the summary lacks the release"; exit 1; }
if bash "$REL/check-stage.sh" "$T/stage-linux" 9.9.9 linux >/dev/null 2>&1; then echo "FAIL check-stage accepted a wrong version"; exit 1; fi
echo "test-dry-run: ok"
