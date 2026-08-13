# Agility SDK (D3D12 redistributable)

Vendored so NRI's D3D12 backend can enable `NRI_ENABLE_AGILITY_SDK_SUPPORT`
(NRI Phase 1, Task 3 of
`.superpowers/sdd/2026-08-13-nri-phase1-substrate/`) — without it,
`ID3D12DeviceBest` resolves to `ID3D12Device8` (`ThirdParty/NRI/Source/D3D12/DeviceD3D12.h:5-9`),
the `OPTIONS9..22` feature queries are compiled out
(`ThirdParty/NRI/Source/D3D12/DeviceD3D12.hpp:484-586`), and enhanced
barriers (`options12.EnhancedBarriersSupported`) can never be true
(capability contract `docs/plans/2026-08-12-nri-capability-contract.md` §2.3).

- **Upstream:** https://www.nuget.org/packages/Microsoft.Direct3D.D3D12
  (NuGet `.nupkg` is a zip; `build/native/` holds headers + `bin/<arch>/`
  holds the redistributable runtime).
- **Pinned version:** `1.619.3`.
- **License:** Microsoft Software License Terms (`LICENSE.txt`, copied
  verbatim from the package) — **not** MIT/permissive like most of this
  repo's vendored deps. Redistribution of `D3D12Core.dll` /
  `d3d12SDKLayers.dll` alongside an app is explicitly permitted (the
  package's own `distributable files.txt` lists exactly these two DLLs
  plus `d3dconfig.exe`, which we do not vendor — we don't ship the config
  tool).
- **Vendored files:** `x64/D3D12Core.dll`, `x64/d3d12SDKLayers.dll` only
  (deliberately **not** `bin/x64/…`, mirroring the package's own layout —
  this repo's root `.gitignore` has an unanchored `bin/` rule for premake
  build output that would silently swallow a `ThirdParty/AgilitySDK/bin/`
  directory; `ThirdParty/tools/dxc/` sidesteps the same trap by keeping its
  vendored binaries un-nested). **No headers are vendored here** — see
  below.

## Version choice

NRI v180's own (CMake-only, unused by our premake build) default is
Agility `1.619.3` — `.example/NRI/CMakeLists.txt:46-47`:

```
set(NRI_AGILITY_SDK_VERSION_MAJOR "619" ...)  # 719 ("D3D12_ERROR_INVALID_REDIST" if Windows developer mode is not enabled)
set(NRI_AGILITY_SDK_VERSION_MINOR "3" ...)    # 1-preview
```

i.e. upstream's own comments flag major `719` as requiring Windows
developer mode (preview-grade redistribution behavior) and minor `1` as a
preview build. `619.3` is the newest release NRI's own maintainers
shipped as the non-preview, no-developer-mode-required default — exactly
"the newest release the contract doesn't warn against" per the capability
contract's Agility paragraph (§2.3, "Agility SDK assumptions"). We took
that value verbatim rather than reaching for whatever is newest on
nuget.org today, since going past it trades a documented-safe default for
an unvetted one with no corresponding upside for this task (the
enhanced-barriers floor `EnhancedBarriersSupported` and `ID3D12Device15`
are both already satisfied at major 619).

## Header comparison — vendored, but found to be a non-issue

`ThirdParty/DirectX-Headers` (pinned `v1.619.1` per `ThirdParty/README.md`)
already carries the **byte-identical** `d3d12.h` (mod CRLF-vs-LF line
endings) as this package's `build/native/include/d3d12.h`: both define
`D3D12_SDK_VERSION ( 619 )` (`d3d12.h:1368` in both trees), both declare
`ID3D12Device15` (`d3d12.h:29696` in both), and `diff` after normalizing
line endings is empty. This makes sense — DirectX-Headers' own versioning
tracks the Agility SDK's major number, and the public C++ interface
surface does not change across an Agility major version's minor/patch
redistributable bumps (619.1 → 619.3 changed the runtime DLLs, not the
header).

Consequence: **we do not add this package's `build/native/include/` to
any include path.** `ThirdParty/NRI/Source/D3D12/SharedD3D12.h:9`'s
`static_assert(D3D12_SDK_VERSION >= 3, ...)` is already satisfied by the
pre-existing DirectX-Headers vendoring — there is no "companion define"
needed beyond flipping `NRI_ENABLE_AGILITY_SDK_SUPPORT` itself in
`ThirdParty/NRI/premake5.lua` (NRI's `NRI_AGILITY_SDK_VERSION_MAJOR` is a
CMake-only symbol used to generate `Include/NRIAgilitySDK.h`, a file our
premake build never generates — grep confirms it appears nowhere in
`ThirdParty/NRI/Source/` or `ThirdParty/NRI/Include/`; the capability
contract's own §6 item 3 makes the same observation). Adding this
package's headers to an include path alongside DirectX-Headers' identical
copy would only risk duplicate-definition conflicts for zero benefit — so
we deliberately vendor **binaries only**.

## Runtime wiring (Task 3)

The D3D12 loader reads `D3D12SDKVersion` / `D3D12SDKPath` as **exported
symbols on the process's main EXE**, not from any DLL — so each host exe
(`ArcaneRuntime`, `ArcaneEditor`, `ArcaneTests`) exports them directly in
its `main.cpp` (see those files for the exact block, mirrored verbatim
from the task brief). `D3D12SDKPath` points at `.\D3D12\`, and
`ThirdParty/NRI/premake5.lua`'s postbuild step for each of those three
projects copies `D3D12Core.dll` + `d3d12SDKLayers.dll` from here into
`<exedir>/D3D12/`.

The empirical proof that the redistributable actually loaded — NRI
logging `"Using ID3D12Device10+"` (`DeviceD3D12.hpp:255`, contract §6 item
3) — is deliberately deferred to the desk milestone (Task 10); this task's
deliverable is the exports + DLL placement + `NRI_ENABLE_AGILITY_SDK_SUPPORT=1`
flip, proven by both configs building and the full gate staying green.
