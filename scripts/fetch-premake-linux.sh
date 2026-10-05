#!/bin/sh
# Fetches the Linux premake5 release into ThirdParty/premake5/premake5
# (gitignored) -- the POSIX twin of the vendored ThirdParty/premake5/premake5.exe.
# Arcane::Toolchain::ResolvePremake already looks for exactly this file on
# POSIX (bundled copy first, then PATH), so arcbuild, the editor's module
# build and the [build-generator] tests find the same premake a Windows
# checkout does. Same version as the vendored .exe (5.0.0-beta8); override
# with PREMAKE_VERSION.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/ThirdParty/premake5/premake5"
# The pinned release and the SHA-256 of its linux tarball (bump together).
# Overriding PREMAKE_VERSION requires PREMAKE_SHA256 for that tarball too.
PREMAKE_PINNED_VERSION=5.0.0-beta8
PREMAKE_PINNED_SHA256=63edd3e7461eebdd45b500a3c7e8ad4e7a67d68f230010f9a97cbb71b4ec59c8
PREMAKE_VERSION="${PREMAKE_VERSION:-$PREMAKE_PINNED_VERSION}"
if [ -z "${PREMAKE_SHA256:-}" ]; then
    if [ "$PREMAKE_VERSION" != "$PREMAKE_PINNED_VERSION" ]; then
        echo "fetch-premake-linux: PREMAKE_VERSION=$PREMAKE_VERSION is not the pinned $PREMAKE_PINNED_VERSION; set PREMAKE_SHA256 to its tarball's SHA-256" >&2
        exit 1
    fi
    PREMAKE_SHA256="$PREMAKE_PINNED_SHA256"
fi
URL="https://github.com/premake/premake-core/releases/download/v$PREMAKE_VERSION/premake-$PREMAKE_VERSION-linux.tar.gz"

if [ -x "$DEST" ] && [ "${FORCE:-0}" != "1" ]; then
    echo "fetch-premake-linux: already present ($("$DEST" --version))"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "fetch-premake-linux: $URL"
curl -fsSL --retry 4 -o "$TMP/premake.tar.gz" "$URL"
if ! (cd "$TMP" && echo "$PREMAKE_SHA256  premake.tar.gz" | sha256sum -c -); then
    echo "fetch-premake-linux: SHA-256 MISMATCH for $URL (expected $PREMAKE_SHA256) -- refusing to install" >&2
    exit 1
fi
tar -xzf "$TMP/premake.tar.gz" -C "$TMP"
install -m 0755 "$TMP/premake5" "$DEST"
"$DEST" --version
