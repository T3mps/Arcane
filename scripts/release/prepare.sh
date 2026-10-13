#!/usr/bin/env bash
# prepare.sh -- decides what a release run is (spec s4) and writes it to
# $GITHUB_OUTPUT: version, channel, mode, prefix, released_at, notes, ci_image.
#   push of tag v*        -> mode publish: the tag must match Version.hpp,
#                            docs/releases/<version>.md must exist, and the
#                            Linux CI image must already be published.
#   dispatch dry-run      -> a fixture build, nothing uploaded (any branch).
#   dispatch rehearsal    -> a fixture build published to staging/ (main only).
# released_at is now: the run's start, shared by the archives and the manifest.
set -euo pipefail
die() { echo "prepare: $*" >&2; exit 1; }
event=${GITHUB_EVENT_NAME:?} ref=${GITHUB_REF:?} out=${GITHUB_OUTPUT:?}
here=$(cd "$(dirname "$0")" && pwd)
released_at=${RELEASED_AT_OVERRIDE:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}
ci_image=""
case $event in
    push)
        [[ $ref == refs/tags/v* ]] || die "a release runs only on v* tags, not $ref"
        header=${VERSION_HPP:-$(git ls-files '*/Arcane/Version.hpp' | head -1)}
        [ -n "$header" ] || die "no Arcane/Version.hpp in the checkout"
        version=$(bash "$here/check-version.sh" "${ref#refs/tags/}" "$header")
        notes="${NOTES_DIR:-docs/releases}/$version.md"
        [ -s "$notes" ] || die "missing or empty $notes: the release notes ship with the tag"
        mode=publish prefix=""
        ci_image=$(bash "$here/resolve-ci-image.sh") ;;
    workflow_dispatch)
        mode=${INPUT_MODE:?}
        case $mode in
            dry-run) ;;
            rehearsal) [ "$ref" = refs/heads/main ] || die "a rehearsal runs only from main" ;;
            *) die "unknown mode $mode" ;;
        esac
        version="0.0.0-preview.${GITHUB_RUN_NUMBER:?}" prefix="staging/" notes="$here/fixtures/rehearsal-notes.md" ;;
    *) die "unexpected event $event" ;;
esac
channel=stable
[[ $version == *-preview.* ]] && channel=preview
{
    echo "version=$version"
    echo "channel=$channel"
    echo "mode=$mode"
    echo "prefix=$prefix"
    echo "released_at=$released_at"
    echo "notes=$notes"
    echo "ci_image=$ci_image"
} >>"$out"
