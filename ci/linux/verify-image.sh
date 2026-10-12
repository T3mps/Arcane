#!/usr/bin/env bash
# verify-image.sh -- checks a built arcane-ci image holds everything the
# hosted Linux legs (.github/workflows/ci.yml) and scripts/ci/local.ps1 rely
# on. Runs INSIDE the image as its default user (root):
#   docker run --rm -v "$PWD/ci/linux:/v:ro" <image> bash /v/verify-image.sh
# scripts/ci/ci-image.sh runs it after every image build. Not an image input:
# it is never copied into the image, so editing it does not change the tag.
set -uo pipefail
fails=0
check() {
    local name=$1; shift
    if "$@" > /tmp/verify.out 2>&1; then
        echo "ok   $name"
    else
        echo "FAIL $name: $(head -c 400 /tmp/verify.out)"
        fails=$((fails + 1))
    fi
}
has() { command -v "$1" > /dev/null; }

check "default user is root" test "$(id -u)" = 0
check "g++-14 is GCC 14" sh -c 'g++-14 -dumpfullversion | grep -q "^14\."'
check "clang++-19 is Clang 19" sh -c 'clang++-19 --version | grep -q "version 19\."'
check "premake 5.0.0-beta8" sh -c '"${ARCANE_TOOLS_DIR:-}/premake5/premake5" --version | grep -q "5.0.0-beta8"'
check "dxc runs" "${ARCANE_TOOLS_DIR:-}/dxc-linux/bin/dxc" --version
check "libdxcompiler.so present" test -f "${ARCANE_TOOLS_DIR:-}/dxc-linux/lib/libdxcompiler.so"
check "dxc headers present" test -d "${ARCANE_TOOLS_DIR:-}/dxc-linux/include/dxc"
check "SDL3 3.2.22 via pkg-config" sh -c 'test "$(pkg-config --modversion sdl3)" = 3.2.22'
check "SDL3_ROOT=/usr/local" test "${SDL3_ROOT:-}" = /usr/local
check "validation layer 1.4.328" grep -q '"api_version": "1.4.328' /usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json
check "no distro validation layer" test ! -e /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json
check "lavapipe ICD" test -f /usr/share/vulkan/icd.d/lvp_icd.json
check "clang-19 sanitizer runtimes" sh -c 'find /usr/lib/llvm-19/lib/clang -name "libclang_rt.asan*" | grep -q . && find /usr/lib/llvm-19/lib/clang -name "libclang_rt.tsan*" | grep -q .'
for t in make cmake ninja pkg-config xvfb-run xauth weston weston-screenshooter xdotool import xwininfo \
         python3 ccache rsync git curl vulkaninfo; do
    check "tool $t" has "$t"
done
for c in gcc-14 g++-14 clang-19 clang++-19; do
    check "ccache link $c" sh -c "test \"\$(readlink -f /usr/lib/ccache/$c)\" = \"\$(readlink -f /usr/bin/ccache)\""
done
check "user arcane is uid 1000" test "$(id -u arcane)" = 1000
check "no ubuntu user" sh -c '! id ubuntu'
check "/work owned by 1000" test "$(stat -c %u /work)" = 1000
check "/ccache owned by 1000" test "$(stat -c %u /ccache)" = 1000
check "root runtime dir" test -d /run/user/0
check "no Arcane source baked in" test ! -e /work/premake5.lua
check "apt pinned to the snapshot" grep -q 'APT::Snapshot' /etc/apt/apt.conf.d/50arcane-snapshot
# The GPU lane's riskiest assumptions, as root (R13): lavapipe under Xvfb,
# and a headless weston (the windowed smoke's Wayland compositor).
check "Xvfb + lavapipe Vulkan as root" sh -c 'VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a vulkaninfo --summary | grep -qi llvmpipe'
check "weston headless starts as root" sh -c '
    d=$(mktemp -d); chmod 700 "$d"
    XDG_RUNTIME_DIR=$d weston --backend=headless --renderer=pixman --socket=probe --idle-time=0 > /tmp/weston.log 2>&1 &
    p=$!
    for _ in $(seq 50); do [ -S "$d/probe" ] && break; sleep 0.1; done
    rc=1; [ -S "$d/probe" ] && rc=0
    kill "$p" 2> /dev/null; cat /tmp/weston.log; exit $rc'

echo
if [ "$fails" -eq 0 ]; then echo "verify-image: all checks passed"; else echo "verify-image: $fails check(s) failed"; fi
[ "$fails" -eq 0 ]
