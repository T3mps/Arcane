#!/usr/bin/env bash
# install-aws-cli.sh -- the pinned aws-cli v2 on a Linux runner (spec s12: the
# version is pinned, and lib.sh sets the checksum settings R2 needs).
set -euo pipefail
# SHA256 is of the zip whose PGP signature (.zip.sig) verified against the AWS CLI
# Team key FB5DB77FD5C118B80511ADA8A6310ACC4672475C when it was pinned.
VERSION=2.37.13
SHA256=c92fde59827622889d3d38a44e2089a137894ce151a2c658a3988ecde4860caa
dir="${RUNNER_TEMP:?}/awscli"
curl -fsSLo "$dir.zip" "https://awscli.amazonaws.com/awscli-exe-linux-x86_64-${VERSION}.zip"
echo "$SHA256  $dir.zip" | sha256sum -c -
unzip -q -o "$dir.zip" -d "$dir"
"$dir/aws/install" --install-dir "$dir/install" --bin-dir "$dir/bin" --update
echo "$dir/bin" >>"${GITHUB_PATH:?}"
"$dir/bin/aws" --version | grep -q "aws-cli/$VERSION" || { echo "aws-cli is not $VERSION" >&2; exit 1; }
