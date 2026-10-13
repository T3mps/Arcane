#!/usr/bin/env bash
# resolve-ci-image.sh -- prints plan A's Linux CI image pinned by digest. The
# release workflow never builds the image: it must already be published by
# ci.yml's ci-image job on main (spec s4, Runners). Fails closed otherwise.
set -euo pipefail
die() { echo "resolve-ci-image: $*" >&2; exit 1; }
IMAGE=ghcr.io/starworksdev/arcane-ci
script=${IMAGE_TAG_SCRIPT:-scripts/ci/image-tag.sh}
[ -f "$script" ] || die "$script is missing (plan A, the local CI pipeline, must be on this branch)"
tag=$(sh "$script") || die "$script failed"
digest=$(docker buildx imagetools inspect "$IMAGE:$tag" --format '{{json .Manifest}}' 2>/dev/null | jq -r '.digest // empty') || digest=""
[ -n "$digest" ] || die "$IMAGE:$tag is not published; ci.yml's ci-image job on main publishes it. The release workflow never builds it."
echo "$IMAGE@$digest"
