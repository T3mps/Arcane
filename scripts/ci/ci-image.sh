#!/usr/bin/env bash
# ci-image.sh <image-repo> <tag> -- the body of ci.yml's ci-image job: make
# sure <image-repo>:<tag> is published, then print its digest (the Linux legs
# run on image@digest, never on the tag).
#   published         -> reuse it
#   SAME_REPO=true    -> build it (ci/linux/Dockerfile, the scripts/ build
#                        context), push it, and run ci/linux/verify-image.sh in it
#   otherwise (a fork pull request, or SAME_REPO unset) -> fail, naming the tag:
#                        a fork cannot publish. The remedy runs fork-authored
#                        Dockerfile/script content in the one job holding
#                        packages: write, so a maintainer REVIEWS the ci/linux/
#                        and scripts/ diff first, then runs the workflow from a
#                        branch of this repository (docs/ci.md, "Image").
# Appends digest=<sha256:...> to $GITHUB_OUTPUT when set.
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
image="${1:?usage: ci-image.sh <image-repo> <tag>}"
tag="${2:?usage: ci-image.sh <image-repo> <tag>}"
ref="$image:$tag"

if docker buildx imagetools inspect "$ref" > /dev/null 2>&1; then
    echo "ci-image: $ref is published"
elif [ "${SAME_REPO:-false}" = true ]; then
    echo "ci-image: $ref is not published -- building it"
    docker buildx build --target build --provenance=false --push \
        --build-context scripts="$REPO_ROOT/scripts" \
        -f "$REPO_ROOT/ci/linux/Dockerfile" -t "$ref" "$REPO_ROOT/ci/linux"
    docker run --rm -v "$REPO_ROOT/ci/linux:/v:ro" "$ref" bash /v/verify-image.sh
else
    echo "::error::$ref is not published, and a fork pull request cannot publish it. A maintainer reviews the ci/linux/ and scripts/ changes, then runs this workflow from a branch of this repository (docs/ci.md, Image)." >&2
    exit 1
fi

digest=$(docker buildx imagetools inspect "$ref" --format '{{json .Manifest}}' \
    | grep -o '"digest":"sha256:[0-9a-f]\{64\}"' | head -n 1 | cut -d'"' -f4 || true)
case "$digest" in
    sha256:*) ;;
    *) echo "::error::could not read the digest of $ref" >&2; exit 1 ;;
esac
echo "ci-image: $ref = $digest"
if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "digest=$digest" >> "$GITHUB_OUTPUT"; fi
