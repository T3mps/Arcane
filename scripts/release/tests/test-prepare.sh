#!/usr/bin/env bash
# Tests for check-version.sh, resolve-ci-image.sh and prepare.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REL=$(cd "$HERE/.." && pwd)
T=$(mktemp -d)
pass=0 fail=0
accepts() { local name=$1; shift; if "$@" >/dev/null 2>&1; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); echo "FAIL $name"; fi; }
refuses() { local name=$1; shift; if "$@" >/dev/null 2>&1; then fail=$((fail + 1)); echo "FAIL $name (accepted)"; else pass=$((pass + 1)); echo "ok   $name"; fi; }
same() { if [ "$2" = "$3" ]; then pass=$((pass + 1)); echo "ok   $1"; else fail=$((fail + 1)); echo "FAIL $1: got '$2', want '$3'"; fi; }

header() { # MAJOR MINOR PATCH PRERELEASE
    cat >"$T/Version.hpp" <<EOF
    inline constexpr int kVersionMajor = $1;
    inline constexpr int kVersionMinor = $2;
    inline constexpr int kVersionPatch = $3;
    inline constexpr const char* kVersionPrerelease = "$4";
EOF
}
cv() { bash "$REL/check-version.sh" "$1" "$T/Version.hpp"; }

# --- check-version
header 0 3 1 ""
accepts "a stable tag matching the header"    cv v0.3.1
same    "it prints the version"               "$(cv v0.3.1)" 0.3.1
refuses "another patch"                       cv v0.3.2
refuses "a preview tag against a stable header" cv v0.3.1-preview.1
refuses "not MAJOR.MINOR.PATCH"               cv v0.3
refuses "a leading zero"                      cv v0.03.1
refuses "a beta prerelease"                   cv v0.3.1-beta.1
header 0 4 0 "preview.2"
accepts "a preview tag matching the header"   cv v0.4.0-preview.2
refuses "the stable tag of a preview header"  cv v0.4.0
printf '    inline constexpr int kVersionMajor = 0;\n    inline constexpr int kVersionMinor = 1;\n' >"$T/Version.hpp"
refuses "today's header (no patch, no prerelease)" cv v0.1.0

# --- resolve-ci-image
mkdir -p "$T/bin"
printf '#!/bin/sh\necho build-0123456789abcdef\n' >"$T/image-tag.sh"
cat >"$T/bin/docker" <<'EOF'
#!/bin/sh
# fake: docker buildx imagetools inspect REF --format ...; published only if REF = $FAKE_PUBLISHED
if [ "$4" = "${FAKE_PUBLISHED:-}" ]; then echo '{"digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}'; else echo "ERROR: not found" >&2; exit 1; fi
EOF
chmod +x "$T/bin/docker"
export PATH="$T/bin:$PATH" IMAGE_TAG_SCRIPT="$T/image-tag.sh"
export FAKE_PUBLISHED=ghcr.io/starworksdev/arcane-ci:build-0123456789abcdef
same    "a published image resolves to its digest" "$(bash "$REL/resolve-ci-image.sh")" \
        "ghcr.io/starworksdev/arcane-ci@sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
export FAKE_PUBLISHED=nothing
refuses "an unpublished image fails closed"   bash "$REL/resolve-ci-image.sh"
refuses "a missing image-tag script fails"    env IMAGE_TAG_SCRIPT="$T/nope.sh" bash "$REL/resolve-ci-image.sh"

# --- prepare
export FAKE_PUBLISHED=ghcr.io/starworksdev/arcane-ci:build-0123456789abcdef
mkdir -p "$T/notes"
prep() { : >"$T/out"; GITHUB_OUTPUT="$T/out" VERSION_HPP="$T/Version.hpp" NOTES_DIR="$T/notes" RELEASED_AT_OVERRIDE=2026-10-12T00:00:00Z bash "$REL/prepare.sh"; }
out() { grep "^$1=" "$T/out" | tail -1 | cut -d= -f2-; }

header 0 3 1 ""
refuses "a tag without its notes file"        env GITHUB_EVENT_NAME=push GITHUB_REF=refs/tags/v0.3.1 bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
echo "Notes." >"$T/notes/0.3.1.md"
accepts "a tag with its notes"                env GITHUB_EVENT_NAME=push GITHUB_REF=refs/tags/v0.3.1 bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
same "  version" "$(out version)" 0.3.1
same "  mode" "$(out mode)" publish
same "  channel" "$(out channel)" stable
same "  prefix" "$(out prefix)" ""
same "  released_at" "$(out released_at)" 2026-10-12T00:00:00Z
same "  ci_image" "$(out ci_image)" "ghcr.io/starworksdev/arcane-ci@sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
accepts "a dry run from any branch"           env GITHUB_EVENT_NAME=workflow_dispatch GITHUB_REF=refs/heads/topic GITHUB_RUN_NUMBER=7 INPUT_MODE=dry-run bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
same "  version" "$(out version)" 0.0.0-preview.7
same "  prefix" "$(out prefix)" staging/
same "  channel" "$(out channel)" preview
refuses "a rehearsal from a branch"           env GITHUB_EVENT_NAME=workflow_dispatch GITHUB_REF=refs/heads/topic GITHUB_RUN_NUMBER=7 INPUT_MODE=rehearsal bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
accepts "a rehearsal from main"               env GITHUB_EVENT_NAME=workflow_dispatch GITHUB_REF=refs/heads/main GITHUB_RUN_NUMBER=8 INPUT_MODE=rehearsal bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
same "  mode" "$(out mode)" rehearsal
refuses "an unsigned rehearsal from a branch"  env GITHUB_EVENT_NAME=workflow_dispatch GITHUB_REF=refs/heads/topic GITHUB_RUN_NUMBER=9 INPUT_MODE=rehearsal-unsigned bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
accepts "an unsigned rehearsal from main"      env GITHUB_EVENT_NAME=workflow_dispatch GITHUB_REF=refs/heads/main GITHUB_RUN_NUMBER=9 INPUT_MODE=rehearsal-unsigned bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
same "  mode" "$(out mode)" rehearsal-unsigned
same "  prefix" "$(out prefix)" staging/
same "  version" "$(out version)" 0.0.0-preview.9
refuses "a pull_request event"                env GITHUB_EVENT_NAME=pull_request GITHUB_REF=refs/pull/1/merge bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
refuses "a branch push"                       env GITHUB_EVENT_NAME=push GITHUB_REF=refs/heads/main bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"

echo "test-prepare: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
