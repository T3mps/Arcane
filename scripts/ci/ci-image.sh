#!/usr/bin/env bash
# ci-image.sh <image-repo> <tag> -- the body of ci.yml's ci-image job: make
# sure <image-repo>:<tag> is published, then print its digest (the Linux legs
# run on image@digest, never on the tag).
#   published         -> reuse it
#   SAME_REPO=true    -> build it into the local store (ci/linux/Dockerfile, the
#                        scripts/ build context, --load), run ci/linux/verify-image.sh
#                        in it, and only THEN push it: a failed verify publishes
#                        nothing, so no later run can reuse an unverified tag
#   otherwise (a fork pull request, or SAME_REPO unset) -> fail, naming the tag:
#                        a fork cannot publish. The remedy runs fork-authored
#                        Dockerfile/script content in the one job holding
#                        packages: write, so a maintainer REVIEWS the ci/linux/
#                        and scripts/ diff first, then runs the workflow from a
#                        branch of this repository (docs/ci.md, "Image").
# The digest is the SHA-256 of the registry's raw manifest bytes
# (`imagetools inspect --raw`): format-agnostic, unlike parsing buildx's
# pretty-printed JSON. Appends digest=<sha256:...> to $GITHUB_OUTPUT when set.
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
image="${1:?usage: ci-image.sh <image-repo> <tag>}"
tag="${2:?usage: ci-image.sh <image-repo> <tag>}"
ref="$image:$tag"

if docker buildx imagetools inspect "$ref" > /dev/null 2>&1; then
    echo "ci-image: $ref is published"
elif [ "${SAME_REPO:-false}" = true ]; then
    echo "ci-image: $ref is not published -- building, verifying, then pushing it"
    docker buildx build --target build --provenance=false --load \
        --build-context scripts="$REPO_ROOT/scripts" \
        -f "$REPO_ROOT/ci/linux/Dockerfile" -t "$ref" "$REPO_ROOT/ci/linux"
    if ! docker run --rm -v "$REPO_ROOT/ci/linux:/v:ro" "$ref" bash /v/verify-image.sh; then
        echo "::error::$ref failed ci/linux/verify-image.sh -- not pushed" >&2
        exit 1
    fi
    docker push "$ref"
else
    echo "::error::$ref is not published, and a fork pull request cannot publish it. A maintainer reviews the ci/linux/ and scripts/ changes, then runs this workflow from a branch of this repository (docs/ci.md, Image)." >&2
    exit 1
fi

raw=$(mktemp)
trap 'rm -f "$raw"' EXIT
docker buildx imagetools inspect --raw "$ref" > "$raw" || true
if [ ! -s "$raw" ]; then
    echo "::error::could not read the manifest of $ref" >&2
    exit 1
fi
if command -v sha256sum > /dev/null 2>&1; then sum=$(sha256sum "$raw"); else sum=$(shasum -a 256 "$raw"); fi
digest="sha256:${sum%% *}"
echo "ci-image: $ref = $digest"
if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "digest=$digest" >> "$GITHUB_OUTPUT"; fi
