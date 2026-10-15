#!/usr/bin/env bash
# check-version.sh TAG HEADER -- prints the version if the tag (vMAJOR.MINOR.PATCH
# or vMAJOR.MINOR.PATCH-preview.N) matches Arcane/Version.hpp (spec s4). Phase 1
# adds kVersionPatch and kVersionPrerelease; until then every tag fails here.
set -euo pipefail
die() { echo "check-version: $*" >&2; exit 1; }
tag=${1:?usage: check-version.sh TAG HEADER}
header=${2:?usage: check-version.sh TAG HEADER}
n='(0|[1-9][0-9]*)'
[[ $tag =~ ^v$n\.$n\.$n(-preview\.$n)?$ ]] || die "tag $tag is not vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-preview.N"
version=${tag#v}
num() { sed -n "s/.*inline constexpr int $1 = \([0-9][0-9]*\);.*/\1/p" "$header" | head -1; }
major=$(num kVersionMajor) minor=$(num kVersionMinor) patch=$(num kVersionPatch)
[ -n "$major" ] && [ -n "$minor" ] && [ -n "$patch" ] || die "$header lacks kVersionMajor/kVersionMinor/kVersionPatch (phase 1 adds kVersionPatch)"
grep -q 'kVersionPrerelease = "' "$header" || die "$header lacks kVersionPrerelease (phase 1 adds it; \"\" for a stable release)"
pre=$(sed -n 's/.*kVersionPrerelease = "\([^"]*\)";.*/\1/p' "$header" | head -1)
want="$major.$minor.$patch${pre:+-$pre}"
[ "$want" = "$version" ] || die "tag $tag does not match $header ($want)"
echo "$version"
