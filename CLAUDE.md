# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

## Repository layout

The repo root IS the premake workspace (`premake5.lua` here generates
`Arcane.slnx` here). This repo was extracted 2026-08-11 from the Aphelyon
monorepo with full filtered history; docs under `docs/` may still cite
pre-extraction paths (`Arcane/...`, `docs/superpowers/...`, `../ThirdParty`).

```
ArcaneCore/        # static lib: Net/Types/Logger + shared header-only utils.
                   #   Namespaced include root ArcaneCore/src/Arcane ->
                   #   #include <Arcane/...>. ZERO game references -- keep it that way.
ArcaneClient/      # the engine DLL: Base/Platform/Render/Host (SDL3, NVRHI on
                   #   D3D12+Vulkan, Batcher2D, ACES tonemap, MSDF text, Assets,
                   #   ImGui, enkiTS jobs, Astra ECS runtime, scene save/load,
                   #   plugin host)
ArcaneRuntime/     # standalone runtime host: opens an .arcproj, runs its game
                   #   module; --frames N = scripted GPU-verify; F5/F6 hot reload
ArcaneEditor/      # the ImGui-on-NVRHI editor host (namespace Arcane::Editor)
ArcaneHub/         # Tauri launcher (Rust): .arcproj association + recents
ArcaneServer/      # server-side engine tooling (scaffold)
ArcaneTests/       # Catch2 suite + HotReloadPluginV1/V2/Bad fixture DLLs
ReferenceProject/  # in-repo sample game, built like an EXTERNAL project via the
                   #   SDK (build/arcane.lua) -> Binaries/ReferenceGame.dll
ThirdParty/        # vendored deps (see NOTICE.md); premake5 + dxc binaries
build/arcane.lua   # the SDK premake module external games consume (ARCANE_SDK)
data/              # shaders (HLSL sources -> compiled into data/shaders/generated/),
                   #   fonts, EngineConfig
docs/              # specs, plans, audits (engine copy of the monorepo docs)
vcpkg-triplets/    # x64-windows-static-md overlay triplet (v143 toolset) for SDL3
```

## Build

```bat
scripts\setup-vcpkg-deps.bat   # once: SDL3 via the overlay triplet (VCPKG_ROOT required)
GenerateProjects.bat            # premake5 -> Arcane.slnx
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe             # run FROM the exe dir
bin\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject --frames 180
```

- Re-run `GenerateProjects.bat` whenever `premake5.lua`, any
  `ThirdParty/*/premake5.lua`, or the file list changes.
- ReferenceProject builds separately (its own workspace over the SDK):
  `cd ReferenceProject && ..\ThirdParty\premake5\premake5.exe vs2026 && msbuild ReferenceProject.slnx /p:Configuration=Debug /m`.
  Build `ReferenceGame.dll` before the ArcaneRuntime `--frames` GPU-verify.
- Three configurations: Debug / Release / Dist (Dist = no symbols, no
  TRACY_ENABLE).

## Rules baked into the workspace

- **/MD everywhere** (dynamic CRT) -- memory crosses the
  ArcaneClient.dll/Game.dll boundary, so all modules share one heap.
  ThirdParty wrappers are parameterized via `THIRDPARTY_STATICRUNTIME` /
  `THIRDPARTY_PROJECT_LOCATION` (see `ThirdParty/README.md`).
- **No `/fp:fast`** in engine builds (determinism rule). UTF-8 without BOM,
  ASCII comments.
- **Units are MKS** (meters/kg/seconds). Never author pixel-scale content;
  the reference camera maps world->screen at `pixelsPerMeter = 100`.
- **Shaders are data:** HLSL sources in `data/shaders/`, compiled by
  `data/shaders/compile-shaders.bat` (DXC; DXIL+SPIR-V; SPIR-V register
  shifts match `nvrhi::VulkanBindingOffsets`: t=0 s=128 b=256 u=384) via the
  ArcaneClient prebuild step into the gitignored `data/shaders/generated/`.
  `ShaderLibrary` loads by name per backend; `ARCANE_SHADER_DIR` overrides
  for the hot-reload dev loop. All scene rendering is linear into the RGBA16F
  Canvas; only TonemapPass (Narkowicz ACES + true sRGB encode) writes the
  backbuffer. `kPxRange`/`kAtlasSize` are mirrored constants between
  `msdf.hlsl` and `TextSystem.cpp`.
- **One TU per module owns the Vulkan-Hpp dynamic dispatcher storage** --
  `ArcaneClient/src/Arcane/Render/VulkanDispatchStorage.cpp`. Projects define
  `NDEBUG` in Release; vulkan.hpp's dispatcher layout is NDEBUG-conditional
  and must match the nvrhi static lib.
- **ABI bumps are cheap** during engine dev -- never weaken a design to avoid
  a `kGamePluginABIVersion` bump (`ArcaneClient/src/Arcane/Plugin/PluginABI.hpp`).
- The 2D physics engine is **Manifold2D** (github.com/T3mps/Manifold2D),
  vendored at `ThirdParty/Manifold2D` (source-only sync: `include/`+`src/`;
  the vendored `premake5.lua` is THIS repo's consumer wrapper -- never
  overwrite it from the standalone repo). Threading is injected via
  `Arcane/Jobs/ArcaneWorkScheduler.hpp` -> `Manifold2D::IWorkScheduler`.
- The ECS is **Astra**, vendored at `ThirdParty/Astra` -- keep it current
  with the standalone repo (commit there first, then sync).

## Tests

- GPU-touching tests are tagged `[gpu]` -- exclude with `~[gpu]` on machines
  without a capable GPU. GPU tests assert `Arcane::RenderErrorCount() == 0`,
  which latches BOTH nvrhi diagnostics and raw VK validation VUIDs --
  validation noise is a test failure.
- ArcaneTests runs in RANDOM order: capture the seed banner on failure and
  reproduce with `--rng-seed N`. Never construct a bare `Arcane::Runtime rt;`
  in a test. Run the suite FROM the exe directory (data paths are
  exe-relative).
- Crashes AND hangs auto-capture symbolized all-thread stacks + a minidump to
  `<exe dir>/diagnostics/` -- read the .txt before theorizing.

## Engine-as-SDK

External projects set `ARCANE_SDK` to this repo root and consume
`build/arcane.lua` (see README). The include surface is
`$ARCANE_SDK/ArcaneClient/src` + `$ARCANE_SDK/ArcaneCore/src` + header-only
ThirdParty; the import lib is
`$ARCANE_SDK/bin/<cfg>-windows-x86_64-md/ArcaneClient/ArcaneClient.lib`. The
editor's Build -> Rebuild Game Module resolves the bundled premake at
`<sdkRoot>/ThirdParty/premake5/premake5.exe` (`ArcaneEditor/src/ModuleBuild.cpp`).
