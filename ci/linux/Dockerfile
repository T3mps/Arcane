# Arcane's Linux CI build image (docs/ci.md, "Image"): Ubuntu 24.04 with the
# exact toolchain the hosted Linux legs and scripts/ci/local.ps1 build with.
# Tools only -- the Arcane source is never baked in.
#
# Tag: scripts/ci/image-tag.sh hashes this file and every repo file it copies
# (ci/linux/image-inputs.txt; the script refuses a COPY outside that list).
# Repo files come in ONLY through the `scripts` build context, one COPY line:
#   docker build --target build --build-context scripts=scripts \
#       -f ci/linux/Dockerfile ci/linux
# Reproducible inputs: the base is pinned by digest, apt resolves from one
# Ubuntu archive snapshot (APT_SNAPSHOT), and SDL3 / premake / DXC / the
# Vulkan validation layer come from the repo's SHA-256- and commit-pinned
# scripts.
#
# The default user stays root: GitHub's container jobs mount the workspace
# owned by the runner's uid 1001 and expect root. scripts/ci/local.ps1 runs
# containers as arcane (uid 1000), which owns /work and /ccache, so a fresh
# named volume mounted there starts writable.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS build

LABEL org.opencontainers.image.source="https://github.com/StarworksDev/Arcane" \
      org.opencontainers.image.description="Arcane Linux CI build image: Ubuntu 24.04, GCC 14, Clang 19, tools only"

ARG APT_SNAPSHOT=20261012T000000Z
ENV DEBIAN_FRONTEND=noninteractive

# snapshot.ubuntu.com is HTTPS-only and the base image has no TLS roots, so
# ca-certificates comes first, from the archive the base image names -- the
# ONE package outside the snapshot (it supplies TLS roots only). Every later
# apt call resolves from the snapshot (APT::Snapshot).
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates \
 && echo "APT::Snapshot \"${APT_SNAPSHOT}\";" > /etc/apt/apt.conf.d/50arcane-snapshot \
 && rm -rf /var/lib/apt/lists/*

# CI's apt set (formerly ci.yml's "Toolchain (Linux)" step): the compilers,
# SDL3's X11/Wayland build deps, the Vulkan loader + Mesa lavapipe, Xvfb; the
# GPU-lane extras (weston, xdotool, imagemagick, x11-utils, python3); ccache
# and rsync for local runs; git and curl for the pinned fetch/build scripts;
# vulkan-tools for verify-image.sh; libclang-rt-19-dev (the sanitizer runtimes)
# for the ASan/UBSan lane that follows A (audit P1). The grep guard fails the build if apt did
# NOT read the snapshot.
RUN apt-get update \
 && ls /var/lib/apt/lists | grep -q '^snapshot\.ubuntu\.com_' \
 && apt-get install -y --no-install-recommends \
      g++-14 clang-19 make cmake ninja-build pkg-config \
      libclang-rt-19-dev libvulkan-dev mesa-vulkan-drivers vulkan-tools \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
      libxss-dev libxfixes-dev libxtst-dev libxkbcommon-dev \
      libwayland-dev wayland-protocols libdecor-0-dev \
      libdrm-dev libgbm-dev libegl-dev libgl-dev \
      libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
      xvfb xauth \
      weston xdotool imagemagick x11-utils python3 \
      ccache rsync git curl \
 && rm -rf /var/lib/apt/lists/*

COPY --from=scripts --chmod=0755 build-sdl3.sh build-vvl-linux.sh fetch-dxc-linux.sh fetch-premake.sh sha256-check.sh /tmp/ci/scripts/

# SDL3 (Ubuntu 24.04 does not package it) into /usr/local, where pkg-config
# and the loader find it. The image has no unversioned cc/c++, so the SDL3
# and validation-layer builds name GCC 14 (inline: the legs pick CC/CXX
# themselves); SDL3's C ABI serves the clang legs too.
RUN CC=gcc-14 CXX=g++-14 /tmp/ci/scripts/build-sdl3.sh /usr/local && ldconfig

# The [gpu] lane's validation layer (build-vvl-linux.sh says why the distro's
# 1.3.275 cannot be used); its tag must resolve to the pinned commit.
RUN CC=gcc-14 CXX=g++-14 MAKE_JOBS="$(nproc)" VVL_WORK=/tmp/vvl /tmp/ci/scripts/build-vvl-linux.sh /usr/local \
 && ldconfig \
 && rm -rf /tmp/vvl

# premake + DXC land under /opt/arcane-tools; scripts/ci/build.sh links them
# into the checkout's ThirdParty/ locations each run.
RUN /tmp/ci/scripts/fetch-premake.sh \
 && /tmp/ci/scripts/fetch-dxc-linux.sh \
 && mkdir -p /opt/arcane-tools/premake5 \
 && mv /tmp/ci/ThirdParty/premake5/premake5 /opt/arcane-tools/premake5/premake5 \
 && mv /tmp/ci/ThirdParty/tools/dxc-linux /opt/arcane-tools/dxc-linux \
 && rm -rf /tmp/ci

# ccache front-ends for the versioned compilers. Only local runs put
# /usr/lib/ccache on PATH; hosted CI compiles without ccache.
RUN for c in gcc-14 g++-14 clang-19 clang++-19; do ln -sf ../../bin/ccache "/usr/lib/ccache/$c"; done

# arcane (uid 1000) for local runs: the base image's own uid-1000 user goes
# first. /run/user/0 is the root runtime dir the GPU lane may use.
RUN userdel --remove ubuntu \
 && groupadd --gid 1000 arcane \
 && useradd --uid 1000 --gid 1000 --create-home --shell /bin/bash arcane \
 && mkdir -p /work /ccache /run/user/0 /run/user/1000 \
 && chown 1000:1000 /work /ccache /run/user/1000 \
 && chmod 0700 /run/user/0 /run/user/1000

ENV ARCANE_TOOLS_DIR=/opt/arcane-tools \
    SDL3_ROOT=/usr/local

CMD ["bash"]
