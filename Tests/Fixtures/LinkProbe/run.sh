#!/bin/sh
# LinkProbe (engine modularization P5, ruling R11): proves on GNU ld (and ld64)
# the two link rules a monolithic package relies on (build/modules.lua's
# arc_module_package):
#   * whole-archive (--whole-archive / -force_load) keeps a static registrar
#     that nothing references, past dead-code stripping (--gc-sections /
#     -dead_strip);
#   * the forced reference (-u ArcModule_<Name>, _ArcModule_<Name> on ld64)
#     pulls its module's anchor object.
# Usage: run.sh [build-dir]   (default: a temp dir). Exit 0 = both held.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-c++}

"$cxx" -std=c++17 -O2 -ffunction-sections -fdata-sections -c "$here/probe_anchor.cpp" -o "$out/probe_anchor.o"
"$cxx" -std=c++17 -O2 -ffunction-sections -fdata-sections -c "$here/probe_registrar.cpp" -o "$out/probe_registrar.o"
"$cxx" -std=c++17 -O2 -ffunction-sections -fdata-sections -c "$here/probe_main.cpp" -o "$out/probe_main.o"
rm -f "$out/libLinkProbe.a"
ar rcs "$out/libLinkProbe.a" "$out/probe_anchor.o" "$out/probe_registrar.o"

if [ "$(uname -s)" = "Darwin" ]; then
    "$cxx" "$out/probe_main.o" -Wl,-force_load,"$out/libLinkProbe.a" -Wl,-u,_ArcModule_LinkProbe \
        -Wl,-dead_strip -o "$out/LinkProbe"
else
    "$cxx" "$out/probe_main.o" -Wl,--whole-archive "$out/libLinkProbe.a" -Wl,--no-whole-archive \
        -Wl,-u,ArcModule_LinkProbe -Wl,--gc-sections -o "$out/LinkProbe"
fi

# The negative control: the same link WITHOUT whole-archive must drop the
# registrar-only object (mask 1) -- otherwise this probe proves nothing.
if [ "$(uname -s)" = "Darwin" ]; then
    "$cxx" "$out/probe_main.o" "$out/libLinkProbe.a" -Wl,-u,_ArcModule_LinkProbe -Wl,-dead_strip -o "$out/LinkProbeControl"
else
    "$cxx" "$out/probe_main.o" "$out/libLinkProbe.a" -Wl,-u,ArcModule_LinkProbe -Wl,--gc-sections -o "$out/LinkProbeControl"
fi
if "$out/LinkProbeControl"; then
    echo "LinkProbe: control linked the registrar without whole-archive -- probe is vacuous" >&2
    exit 1
fi

"$out/LinkProbe"
