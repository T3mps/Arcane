# NRI (NVIDIA Render Interface)

Vendored from upstream tag `v180` (commit `4b485316463969f182db15e67aad2aec2f40a3d7`,
the same clone tracked at `.example/NRI` in this repo) as the substrate
underneath NVRHI (spec: `docs/plans/2026-08-13-nri-phase1-substrate.md`,
Phase 1). Backends compiled into this static lib: **D3D12, VK, NONE,
Validation, Creation** (the dispatch layer). Vendored files: `Include/`
(including `Extensions/`), `Source/{Shared,D3D12,VK,NONE,Validation,Creation}`,
`Source/NRIConfig.h`, `LICENSE.txt`.

## Excluded

- **`Source/D3D11`, `Source/WGPU`** — backends not needed by this engine
  (D3D11 is retired per the NVRHI wrapper's own `NVRHI_WITH_DX11=0`
  convention; WGPU is unused). `NRI_ENABLE_D3D11_SUPPORT` /
  `NRI_ENABLE_WGPU_SUPPORT` are left undefined in `premake5.lua` so every
  `#if NRI_ENABLE_*_SUPPORT` guard around those backends' headers
  (`Source/Creation/Creation.cpp` in particular) compiles them out.
- **CMake files, tests, samples, `Shaders/`, `Resources/`, `*.natvis`,
  the `1-Deploy`/`2-Build`/`3-PrepareSDK`/`4-Clean` scripts** — build
  infrastructure this repo's premake wrapper replaces outright.
- **NVAPI, AMDAGS, NVTX, NGX/FFX/XeSS upscalers, ImGui extension** —
  upstream `CMakeLists.txt` FetchContent-pulls each of these from
  GitHub/NuGet on demand; none is part of this repo's vendor set, so their
  `NRI_ENABLE_*` macros stay undefined.

**Agility SDK is NOT excluded** — as of Task 3,
`NRI_ENABLE_AGILITY_SDK_SUPPORT=1` and the redistributable is vendored at
`ThirdParty/AgilitySDK/` (binaries only: `D3D12Core.dll` +
`d3d12SDKLayers.dll`; no headers — see that directory's `README.md` for
the version pin and the header-comparison finding that made vendoring
headers unnecessary). This unlocks `ID3D12Device10+`/`ID3D12Device15` and
the `OPTIONS9..22` feature queries (incl. enhanced barriers) in NRI's
D3D12 backend. The three host exes (`ArcaneRuntime`, `ArcaneEditor`,
`ArcaneTests`) export `D3D12SDKVersion`/`D3D12SDKPath` from their own
mains and the workspace `premake5.lua` copies both DLLs into each exe's
`D3D12/` output subdirectory post-build.

## Update procedure

Vendored files are never hand-edited. To pick up a newer NRI release:

1. Re-clone (or update) the reference checkout at `.example/NRI` to the
   target tag/commit.
2. Re-copy `Include/`, `Source/{Shared,D3D12,VK,NONE,Validation,Creation}`,
   `Source/NRIConfig.h`, and `LICENSE.txt` from the fresh clone into this
   directory, replacing the existing copies wholesale (do not diff/merge
   by hand).
3. Diff `Source/D3D12/**`/`Source/VK/**` for new files against
   `premake5.lua`'s `files{}` globs — they're `**` wildcards, so new files
   in already-vendored dirs are picked up automatically; only a *new*
   upstream source directory (e.g. a split-out backend) would need a
   `premake5.lua` edit.
4. Re-run `ThirdParty\premake5\premake5.exe vs2026` from the repo root and
   rebuild both configs.
