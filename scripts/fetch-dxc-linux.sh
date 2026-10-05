#!/bin/sh
# Fetches the Linux DirectX Shader Compiler release into
# ThirdParty/tools/dxc-linux/ (gitignored) -- the Linux counterpart of the
# vendored Windows drop in ThirdParty/tools/dxc/. Two consumers:
#   * data/shaders/compile-shaders.sh (the ArcaneClient prebuild) runs bin/dxc;
#   * the runtime compile service (ShaderCompiler) dlopen()s lib/libdxcompiler.so,
#     which the host/test postbuilds stage beside each exe when present.
# Not vendored: the Linux release is ~32 MB of binaries, and the Windows drop
# (v1.9.2602.24) has no Linux asset name this script could pin blind. Override
# the release with DXC_TAG/DXC_ASSET.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/ThirdParty/tools/dxc-linux"
DXC_TAG="${DXC_TAG:-v1.8.2505.1}"
DXC_ASSET="${DXC_ASSET:-linux_dxc_2025_07_14.x86_64.tar.gz}"
URL="https://github.com/microsoft/DirectXShaderCompiler/releases/download/$DXC_TAG/$DXC_ASSET"

if [ -x "$DEST/bin/dxc" ] && [ -f "$DEST/lib/libdxcompiler.so" ] && [ "${FORCE:-0}" != "1" ]; then
    echo "fetch-dxc-linux: already present at $DEST ($("$DEST/bin/dxc" --version 2>/dev/null || echo unknown))"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "fetch-dxc-linux: $URL"
curl -fsSL --retry 4 -o "$TMP/dxc.tar.gz" "$URL"
rm -rf "$DEST"
mkdir -p "$DEST"
tar -xzf "$TMP/dxc.tar.gz" -C "$DEST"
chmod +x "$DEST/bin/dxc"
"$DEST/bin/dxc" --version
