# Linux GCC/Clang build-blocker inventory: Arcane engine

*Proposed path: `docs/research/2026-10-01-linux-gcc-clang-build-inventory.md`. Measured 2026-10-01 against `github.com/T3mps/Arcane` main @ `57af0cab`. The local engine main may carry unpushed commits on top of that.*

## 0. Environment and caveats

- **Host.** The brief assumed a Linux cloud VM. This session actually ran on the Windows desk, so all work was done in **WSL2 Ubuntu 24.04.2** (kernel 6.18.33.2-microsoft-standard-WSL2): 10 cores, about 8 GB RAM, no sudo.
- **Clone.** Arcane was cloned to `~/Arcane` inside WSL. Nothing was pushed or committed anywhere. The proof fixes sit uncommitted in `~/Arcane-fix`, and the build trees are `~/p2-*` and `~/p3-*`. Logs are under `~/logs/`.
- **Toolchains.** I had no sudo, so GCC 14 and Clang 19 came from Ubuntu `.deb`s via `apt-get download` + `dpkg -x` into `~/opt`.
  - g++-13 (Ubuntu 13.3.0-6ubuntu2~24.04.1), the system default
  - g++-14 14.2.0-4ubuntu2~24.04.1
  - Ubuntu clang 18.1.3, the system default
  - Ubuntu clang 19.1.1, using libstdc++ 14 via `--gcc-install-dir`
  - GNU Make 4.3, ninja 1.12.1, GNU ld 2.42
  - **premake5 5.0.0-beta8**, the official Linux release (same version as the Windows bundled binary)
- **SDL3.** The Linux build gets it from a vcpkg *windows* triplet path. I used SDL3 3.2.22 instead:
  - Headers went into a fake `VCPKG_ROOT=~/fakevcpkg/installed/x64-windows-static-md/include`.
  - For the link probe I built a static `libSDL3.a` with `SDL_UNIX_CONSOLE_BUILD=ON`, because the X11/Wayland dev headers can't be installed without sudo.
- **Not verified on MSVC.** None of the proof fixes was compiled with MSVC here. They are written to be Windows-neutral (guards and `filter "system:not windows"`), but a Windows build of the patch is still owed before anything lands.
- **Upstream-first fixes.** Two fixes touch vendored code that the repo rule says must change upstream first: Astra (`SmallVector.hpp`, `TypeID.hpp`) and NRI (worked around in Arcane code, not in NRI itself).
- **CI sizing.** `make -j8` of the GCC Debug test target ran the 8 GB WSL VM out of memory and the VM died. `-j3`/`-j5` were fine. A Linux CI agent needs ≥16 GB or a low `-j`. Debug outputs are large: `libArcaneCore.so` 115 MB, `libArcaneClient.so` 161 MB, `ArcaneTests` 510 MB.

## 1. Headline

The port is much closer than "never built on Linux" suggests.

- **Before any fix:** 10 of the 12 vendored static libs build on all four compilers. Two root causes cascade into roughly 50,000 diagnostics: `__declspec` in the `IMGUI_API` defines, and the NRI D3D12 backend being built unconditionally.
- **After 2 premake fixes and 5 small source fixes (GCC 14 and Clang 19):**
  - All 12 ThirdParty libs compile with zero errors.
  - ArcaneCore, ArcaneAssetPipeline, arccook, arcbuild, ArcaneServer, ArcaneRuntime and all ArcaneTests TUs compile, except four files.
  - GCC 14 and Clang 19 converge on the **same** four residual files: `NriDevice.cpp`, `EditorAppFrame.cpp`, `MeshNodeTest.cpp`, `CoreDllTest.cpp`.
- **After all 19 proof-fix files (GCC 14 only):**
  - Everything except **ArcaneEditor** (only `EditorAppFrame.cpp` is left, ShellExecuteW) compiles and **links**.
  - arccook cooks ReferenceProject on Linux (`cooked=2 failed=0`).
  - `arcbuild probe` runs.
  - ArcaneServer and ArcaneRuntime run.
  - ArcaneRuntime boots headless on **Vulkan**, with `SDL_VIDEODRIVER=offscreen` and the system `libvulkan.so.1` runtime, through `gpu_core` to `plugin_load`.
  - ArcaneTests `~[gpu]` ran **1022 of 2218 cases** before a SIGSEGV. Failing cases dropped from 91 to **69** after the Astra GCC type-name fix. The failure classes are in §4.
- **Clang 19 status:** it tracked GCC 14 through pass 3. It was **not** re-run after the last batch (the NriDevice guard, the stub TU, MeshNodeTest, CoreDllTest, TypeID).

## 2. Summary table

Counts are distinct root causes, deduplicated.

| # | Category | Distinct blockers | Severity | Est. effort |
|---|---|---|---|---|
| A | premake / toolchain config | 14 | **Blocker** (2 of them cascade to about 50k errors) | 2–3 d |
| B | MSVC-only language extensions | 5 | Blocker (per-TU) | 0.5 d |
| C | MSVC CRT "secure" / wide-char functions, unguarded | 3 (others already guarded) | Blocker (per-TU) | 0.5 d |
| D | Missing / implicit includes (MSVC STL / WinSDK transitivity) | 3 | Blocker (per-TU) | 0.25 d |
| E | Windows-only headers/APIs with no fallback (compile-breaking) | 5 sites | Blocker | 1–2 d |
| E′ | Windows-only behaviour (compiles, silently no-ops) | 92 no-fallback `_WIN32` blocks | Functional, not build | 2–4 wk (Phase 4) |
| F | Intrinsics / SIMD | 0 blockers; 1 determinism risk | Low (risk) | 0.5 d decision |
| G | `__declspec`/visibility and the DLL boundary | 4 issues | Medium (links today; hygiene/ODR) | 1–2 d |
| H | D3D12-only code to exclude (Vulkan stays) | 6 seams | Blocker | 1–2 d |
| I | Third-party libs on Linux | 6 issues (2 runtime) | Mixed | 1–2 d (+ upstream Astra/Mosaic) |
| J | std-library / language-level gaps | 2 (std::expected on Clang 18; deducing-this on GCC 13) | Blocker *for those versions* | 0 (set min versions) |
| K | Link errors | 4 | Blocker | 1 d |
| L | Runtime/semantic (tests) | 69 failing cases in 6 classes + 1 SIGSEGV | Post-build | 1–2 wk |

**Recommended minimum toolchains: GCC 14 and Clang 19, both with libstdc++ 14.**
- GCC 13 can't compile C++23 deducing-this.
- Clang 18 can't use `std::expected` from *any* libstdc++.

## 3. Per-category detail

### A. premake / toolchain config

| Blocker | Evidence |
|---|---|
| **A1 `__declspec` in `IMGUI_API` defines.** `__declspec` is PE/COFF-only. | Defined at `premake5.lua:58,61` (`THIRDPARTY_IMGUI_API` / `THIRDPARTY_NODE_EDITOR_IMGUI_API`) and `:726,920,1065,1775,1846`, plus `build/arcane.lua:210`. GCC: `<command-line>: error: expected constructor, destructor, or type conversion before '(' token`, which caused 24,732 errors in imgui and 5,972 in imgui-node-editor. Clang: `imgui.h:399:5: error: '__declspec' attributes are not enabled; use '-fdeclspec' or '-fms-extensions'`. It cascades into every TU that includes imgui. **Fixed (proof).** |
| **A2 NRI D3D12 backend built on every target.** | `ThirdParty/NRI/premake5.lua:69-71,100,115`. The error is `DirectX-Headers/include/directx/d3d12.h:26:10: fatal error: rpc.h: No such file or directory` and `Source/Shared/SharedExternal.h:17:14: fatal error: dxgi1_6.h`. **Fixed (proof)** by moving the D3D12 files and defines under `filter "system:windows"`. |
| **A3 `VCPKG_ROOT` is mandatory and the path is hard-wired to `installed/x64-windows-static-md`.** | `premake5.lua:11-15,52,85`. On Linux the generate step aborts without `VCPKG_ROOT`. SDL3 has no Linux source in this layout. |
| **A4 Shader prebuild is a `.bat`.** | `premake5.lua:677-679` `call "…/compile-shaders.bat"` runs under `/bin/sh`, and every ArcaneClient object depends on `prebuild` (`Makefile:288 $(OBJECTS): \| prebuild`), so nothing compiles. The script drives `ThirdParty/tools/dxc/dxc.exe`, which is Windows-only. **Gated to Windows (proof).** It needs a POSIX twin over the Linux DXC release. |
| **A5 Postbuild hard-codes PE names.** | Every `{COPYFILE} …/ArcaneCore.dll`, `ArcaneClient.dll`, `ArcaneCrashReporter.exe`, `arccook.exe`, `dxcompiler.dll`, `D3D12Core.dll` line (`premake5.lua:316,402,830-841,928-991,1070-1122,1722+`). Linux error: `cp: cannot stat '../bin/…/ArcaneCore/ArcaneCore.dll'`. This is why every exe "fails" even after a successful link, and why no data is staged beside the tests (see §4 L1). |
| **A6 Static libs aren't PIC.** | `ld: …libenkiTS.a(TaskScheduler.o): relocation R_X86_64_TPOFF32 against '_ZL13gtl_threadNum' can not be used when making a shared object; recompile with -fPIC`. **Fixed (proof)** with workspace-level `pic "On"` for non-Windows targets. |
| **A7 MSVC-numbered `disablewarnings` leak to GCC/Clang.** | The root `4251` warning (`premake5.lua:39`) becomes `-Wno-4251`. Clang then emits `warning: unknown warning option '-Wno-4251' [-Wunknown-warning-option]` once per TU, 46 times in ArcaneCore alone. **Fixed for the root (proof).** The ThirdParty wrappers do the same (freetype `-Wno-4244 -Wno-4267`, msdfgen `-Wno-4005`, NRI `-Wno-4324`); those are still open. |
| **A8 `floatingpoint "Strict"` has no portable meaning.** | GCC gets `-ffloat-store` (an x87-only knob, not `/fp:strict`). Clang gets **no flag at all**, so Clang's default `-ffp-contract=on` plus `-mfma` will fuse multiply-adds. See F1. |
| **A9 SDL3 link line exists only in the Windows filter.** | `premake5.lua:736-744` (`SDL3-static` + Win32 system libs). On Linux, `libArcaneClient.so` links with **84 undefined `SDL_*` symbols** that only surface at the exe link. |
| **A10 `/WHOLEARCHIVE:imgui` has no Linux equivalent.** | `premake5.lua:711-714`, whose own comment already says "Linux port needs --whole-archive". It links today because ELF exports default-visibility symbols, but imgui objects nothing references (e.g. `imgui_demo`) won't be in the .so. |
| **A11 No `-Wl,--no-undefined` / `-z defs` on shared libs.** | ELF `.so` links tolerate undefined symbols, so A9 and K3 appear late, at exe link. |
| **A12 No `-fvisibility=hidden`.** | See G1. |
| **A13 Ninja action hard-codes compiler names.** | `premake5 ninja` on Linux generates fine (`ninja -n` plans 743 edges), but the rules are literally `command = g++ $cxxflags …`, ignoring `CXX`. Only `--cc=clang` switches it. arcbuild's Ninja backend has to pass `--cc`. |
| **A14 Runtime library lookup works only in the dev layout.** | Premake emits `RUNPATH $ORIGIN/../ArcaneCore:$ORIGIN/../ArcaneClient`, which is correct for `bin/<cfg>/<proj>/`. The staged "DLL beside the exe" layout (hosts, packaged editor, game `Binaries/`) needs `$ORIGIN`. Without it, staging copies are pointless and the loader fails. |

### B. MSVC-only language extensions

| Blocker | File:line | Message | Status |
|---|---|---|---|
| B1 `static_cast` between a function pointer and `void*` | `ArcaneCore/src/Arcane/Util/FunctionRef.hpp:35,37` | GCC: `invalid 'static_cast' from type 'int (*)(int, int)' to type 'const void*'`; Clang: `static_cast from 'int (*)(int, int)' to 'const void *' is not allowed` | **Fixed (proof).** The same code exists in `ThirdParty/Mosaic/include/Mosaic/FunctionRef.hpp:38`, which needs an upstream fix. |
| B2 NSDMI-bearing nested class as a default argument | `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp:654` `const NodeSet& nodes = {}` | GCC: `could not convert '<brace-enclosed initializer list>()' … to 'const NriGraphContext::NodeSet&'`; Clang: `default member initializer for 'hostHud' needed within definition of enclosing class` | **Fixed (proof)** with an inline 4-argument overload. Broke 10 TUs. |
| B3 Implicit lambda capture of an odr-used `constexpr` local | `ArcaneClient/src/Arcane/Render/Nri/nodes/FullscreenNodes.cpp:1488` (consts at :1465/:1467) | Clang: `variable 'kOffscreenEntry' cannot be implicitly captured in a lambda with no capture-default specified` | **Fixed (proof)** with `static constexpr`. |
| B4 `extern "C" __declspec(dllexport)` Agility SDK exports in exe mains | `ArcaneRuntime/src/main.cpp:24-25`, `ArcaneEditor/src/main.cpp:42-43`, `ArcaneTests/src/test_main.cpp:16-17` | Clang: `'__declspec' attributes are not enabled` | **Fixed (proof)**, guarded with `_WIN32`. |
| B5 `__declspec` in imgui defines | (= A1) | | Fixed |

### C. MSVC CRT secure / wide functions, unguarded

Other sites already have `_WIN32` guards and compiled fine: `IGpuCrashBackend.hpp:154,224 fopen_s`, `ConsoleModel.cpp:76` / `ExclusionList.cpp:204 localtime_s`, `RecentProjects.cpp:104`, `arcbuild/src/Environment.cpp:24 _wgetenv`, `TestEnvironment.hpp:83 _putenv_s`.

| Site | Message | Status |
|---|---|---|
| `ArcaneCore/src/Arcane/Base/ModuleTable.cpp:57` `strncpy_s(…, _TRUNCATE)` | `'strncpy_s' was not declared in this scope; did you mean 'strncpy'?` / `'_TRUNCATE' was not declared` | **Fixed (proof)** |
| `ArcaneEditor/src/App/EditorApp.cpp:1684` `_wgetenv(L"LOCALAPPDATA")` | `'_wgetenv' was not declared in this scope` | **Fixed (proof)**, XDG twin |
| `ArcaneTests/src/MeshNodeTest.cpp:332,337` `_putenv_s` | `'_putenv_s' was not declared in this scope; did you mean 'putenv'?` | **Fixed (proof)**, setenv/unsetenv |

### D. Missing / implicit includes

| Site | Message | Status |
|---|---|---|
| `ArcaneCore/src/Arcane/Net/TcpSocket.hpp:320-321,405`. The POSIX branch uses `fcntl/F_GETFL/F_SETFL/O_NONBLOCK` without `<fcntl.h>`. This blocks the Gacha Server on Linux too. | `'fcntl' was not declared in this scope` (found by the standalone-header check, then hit in `FramingTest.cpp` and `WireRoundTripTest.cpp`) | **Fixed (proof)** |
| `ThirdParty/Astra/include/Astra/Container/SmallVector.hpp:277` uses `std::numeric_limits` without `<limits>` | `incomplete type 'std::numeric_limits<long unsigned int>' used in nested name specifier` | **Fixed (proof)**. Needs the upstream Astra fix first. |
| NRI `Include/Extensions/NRIWrapperVK.h:110` isn't self-contained: it needs `NRIRayTracing.h`, which only `NRIWrapperD3D12.h` pulls in | `'AccelerationStructureBits' does not name a type` (shows up once D3D12 is excluded) | **Worked around (proof)** in `NriDevice.cpp`. Worth reporting upstream to NRI. |

Standalone header check: 94 ArcaneCore headers compiled alone. After D1/D2, the only failure was `Plugin/GameModule.hpp`, which needs imgui on the include path (expected).

### E. Windows-only APIs with no fallback (compile-breaking)

| Site | Message | Status |
|---|---|---|
| `ArcaneCore/src/Arcane/Base/Diagnostics.cpp:2544`. `kReasonMax` is defined inside the `#if _WIN32` block at :226 but used by the unguarded `FormatReason`. A latent bug that only a non-Windows build could reveal. | `'kReasonMax' was not declared in this scope` | **Fixed (proof)** |
| `ArcaneClient/src/Arcane/Render/DeviceCreationVulkan.cpp:53` `VK_KHR_WIN32_SURFACE_EXTENSION_NAME` | `was not declared in this scope; did you mean 'VK_KHR_SURFACE_EXTENSION_NAME'?` | **Guarded (proof).** The Linux surface extensions should come from `SDL_Vulkan_GetInstanceExtensions`. |
| `ArcaneEditor/src/App/EditorAppFrame.cpp:189,190,204,205,208,209`. The `ShellExecuteW`/`SW_SHOWNORMAL`/`INT_PTR` *calls* are unguarded; only the include at :64-73 is guarded. | `'ShellExecuteW' was not declared in this scope`; `'INT_PTR' does not name a type` | **Open.** This is the only compile error left in ArcaneEditor. Use an `xdg-open`/`SDL_OpenURL` twin. |
| `ArcaneTests/src/CoreDllTest.cpp:25` `#include <windows.h>` (GetModuleHandleEx FROM_ADDRESS) | `fatal error: windows.h: No such file or directory` | **Excluded on non-Windows (proof).** The ELF twin is `dladdr`. |
| `ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp:6,15,111,139-140,278-344,538-547` (D3D12 seam) | `d3d12.h:26:10: fatal error: rpc.h`, then (once guarded) `'DrainD3D12DebugMessages' was not declared` | **Fixed (proof)**. See H. |

### E′. Windows-only behaviour (compiles, no-ops)

Static scan of every `#if` block that mentions `_WIN32`/`_MSC_VER` across the 11 first-party modules: **240 blocks**.
- **59** are pragma-only (`#pragma warning(push/disable:4251)`).
- **86** have an `#else` fallback.
- **3** are negated (POSIX-first).
- **92** have no fallback, meaning the feature silently disappears off-Windows.
  - By module: ArcaneCore 38, ArcaneTests 21, ArcaneEditor 16, ArcaneClient 12, ArcaneServer 3, arcbuild 2.
  - The biggest blocks: `Base/Diagnostics.cpp:464` (1022 lines), `:2243` (274), `:1589` (251), `:213` (152); `BootSplashPresenterTest.cpp:16` (358); `NativeWindowTest.cpp:5` (182); `Helpers/HostWitness.cpp:21` (180); `arcbuild/src/Process.cpp:34` (166); `BuildDriverTest.cpp:1342` (143).

Raw counts:
- 249 `_WIN32|_MSC_VER` lines
- 47 files include `windows.h`
- `std::wstring` in 102 lines / 24 files, `wchar_t` in 59 lines / 27 files
- `CreateProcessW` in 45 lines / 17 files
- **0** uses of `MOSAIC_PLATFORM_*`, even though `Mosaic/Platform.hpp` defines them.

ArcaneCrashReporter, death-fixture and arcbuild-process-fixture are gated Windows-only in premake (`premake5.lua:445,499,588`).

Misleading log on Linux: `Diagnostics armed (crash handler on, hang watchdog on @ 12s, …)` prints from both ArcaneServer and ArcaneRuntime, although Diagnostics no-ops off-Windows.

### F. Intrinsics / SIMD

No compile blockers.
- `-mavx2 -mfma` is applied on Linux (`premake5.lua:27-28`).
- Manifold2D and Mosaic build clean on all four compilers.
- `_BitScan`/`__popcnt` (one file) are already guarded.

**F1, risk:** Clang gets no FP-strictness flag and GCC gets the meaningless `-ffloat-store`. A Clang build with `-mfma` will contract a*b+c into FMA, which can diverge from the MSVC `/fp:strict` results that the physics parity tests and goldens assume. Recommendation: `-ffp-contract=off` (plus `-fno-fast-math`) for gcc/clang wherever `floatingpoint "Strict"` is set.

### G. `__declspec`/visibility and the DLL boundary

- **G1.** `ARCANE_CORE_API` and `ARCANE_API` already have an ELF branch (`Core/Api.hpp:27`, `Base/Api.hpp:15`, `visibility("default")`). But there's no `-fvisibility=hidden`, so **`libArcaneCore.so` exports 59,279 dynamic symbols**. The Windows export audit therefore means nothing on Linux, and missing-`ARCANE_*_API` bugs will hide there.
- **G2.** ArcaneTests links NRI statically (`premake5.lua:1647`) *and* `libArcaneClient.so` contains its own static NRI. On ELF with default visibility, the .so's NRI calls can interpose onto the exe's copy. The Windows "two static copies are isolated" reasoning doesn't hold on Linux. Fix with hidden visibility or `-Bsymbolic`.
- **G3.** imgui single-instance (GImGui) works only by accident of default visibility. When hidden visibility arrives it needs `IMGUI_API=__attribute__((visibility("default")))`, best set via an `IMGUI_USER_CONFIG` header.
- **G4.** `-fdeclspec`/`-fms-extensions` was deliberately *not* used. Spelling per target in premake is cleaner.

### H. D3D12-only code (exclude on Linux; Vulkan stays)

1. NRI D3D12 backend and Agility. **Fixed (A2).**
2. `DeviceCreationD3D12.cpp` and `GpuCrashD3D12.cpp` include `<d3d12.h>` unconditionally. **Excluded (proof)** via `removefiles` under `filter "system:not windows"`.
3. `NriDevice.cpp` seam: the include, the `NativeDeviceOwner` create/destroy, `WrapD3D12`, and the DXGI debug drains. **Guarded (proof).** I used an empty `D3D12DeviceCreation` on non-Windows so the `unique_ptr<>` member stays destructible.
4. Three D3D12 symbols referenced from cross-backend code: `NriDiagnostics.cpp:203,217,844-848` names `ObserveDeviceRemovedD3D12`, `ResetDeviceRemovedLatchD3D12` and `D3D12NativeDeviceRemoved`. ArcaneRuntime's link fails with `undefined reference to Arcane::ObserveDeviceRemovedD3D12()` and the other two. **Fixed (proof)** with a non-Windows twin TU, `Render/DeviceRemovedD3D12Stub.cpp`.
5. Agility SDK exe exports. **Guarded (B4).**
6. **The backend defaults to D3D12** (open):
   - `NriDevice.cpp:95` (`NativeDeviceOwner::Impl::backend`)
   - `NriDevice.hpp:191` (`m_backend`)
   - `CreateNoneForTests` reports `GraphicsBackend::D3D12`
   - `ArcaneRuntime --backend` defaults to `dx12`, so on Linux a run fails unless `--backend vulkan` is passed.

### I. Third-party libs on Linux

- All 12 vendored static libs build on GCC 13/14 and Clang 18/19 once A1, A2 and A6 are applied: Catch2, rapidcheck, enkiTS, TracyClient, freetype, msdfgen, NRI, imgui, imgui-node-editor, Manifold2D, bc7enc_rdo, meshoptimizer.
- **I1, runtime: Astra `TypeNameInternal` is wrong on GCC.** `ThirdParty/Astra/include/Astra/Core/TypeID.hpp:171-188`. GCC's `__PRETTY_FUNCTION__` is `"… [with T = X; std::string_view = std::basic_string_view<char>]"` and the code cuts at `rfind("]")`. Probe output:
  - before: `g++-14: [Arcane::Transform; std::string_view = std::basic_string_view<char>] hash=6e104bf0d20999c`
  - Clang: `[Arcane::Transform] hash=dca2740b541ebac9`
  - after the fix, both: `[Arcane::Transform] hash=dca2740b541ebac9`

  This broke every name-keyed descriptor lookup on GCC. It caused **22 test failures** (CommandStack, component catalog, Inspector vector rows, SceneMigration v5→v6, GameComponents). **Fixed (proof)**; needs the upstream Astra fix first.
- **I2.** Astra `SmallVector.hpp` is missing `<limits>` (D2).
- **I3.** Mosaic `FunctionRef.hpp:38` has the same B1 bug (upstream).
- **I4.** NRI `NRIWrapperVK.h` include-order dependency (D3).
- **I5.** SDL3 comes only from a vcpkg Windows triplet path. Ubuntu 24.04 doesn't package SDL3 (25.04+ does). Linux needs a vcpkg `x64-linux` triplet, or a from-source build plus X11/Wayland/libdecor dev packages.
- **I6.** DXC (runtime `ShaderCompiler`, which `LoadLibrary`s `dxcompiler.dll`) is a Windows-only vendor drop. Linux needs `libdxcompiler.so` and a `dlopen` path. Runtime log: `ArcaneRuntime: dxcompiler.dll unavailable -- sprite materials and the scene post chain will not bind`.

### J. std-library / language gaps

- **J1. `std::expected` is unusable with Clang 18 + any libstdc++.** libstdc++'s `<expected>` requires `__cpp_concepts >= 202002L`; Clang 18 reports `201907L` and Clang 19 reports `202002`.
  - Message: `arcbuild/src/Process.hpp:55:32: error: no template named 'expected' in namespace 'std'`, then cascades.
  - Scope: all 9 arcbuild TUs, plus the arcbuild sources compiled into ArcaneTests, plus `BuildDriverTest.cpp`. That's 96 errors in arcbuild and 140 in ArcaneTests on Clang 18.
  - `<expected>` is used in 4 files.
  - Clang 19 + libstdc++ 14 compiles them clean.
- **J2. GCC 13 lacks C++23 deducing-this (P0847).** `ArcaneEditor/src/Panels/EntityList.cpp:105` `[&](this auto&& self, …)` gives `expected identifier before 'this'`. GCC 14 and Clang 18+ are fine.
- **Not used anywhere** (no exposure): `<print>`, `<stacktrace>`, `<format>`, `<generator>`, `<flat_map>`, `<mdspan>`, `<spanstream>`. libc++ 18 lacks `<stacktrace>`, so it doesn't matter. The libc++ path wasn't exercised further.

### K. Link errors (after compile is clean)

- **K1.** Non-PIC static libs (A6). Fixed.
- **K2.** The postbuild copies `.dll`/`.exe` (A5). Every exe reports `rc=2` even though the binary linked.
- **K3.** 84 undefined `SDL_*` in `libArcaneClient.so` (A9) and 3 undefined D3D12 symbols (H4). For the probe, `LDFLAGS="-Wl,--whole-archive libSDL3.a -Wl,--no-whole-archive"` brought `ldd -r libArcaneClient.so` to **0 undefined**.
- **K4.** The ArcaneTests Debug link needs a lot of memory (510 MB output). Keep `-j` low or use `-Wl,--no-keep-memory`.

## 4. Linux test run (GCC 14, after all proof fixes)

Command: `SDL_VIDEODRIVER=offscreen ./ArcaneTests "~[gpu]" --rng-seed 1 -r junit` from the exe dir. There are 2218 non-GPU cases (2299 total). **1022 ran before a SIGSEGV** in `"ListAssetReferences reads a mesh's material as References…"` (`AssetReferencesTest.cpp:332`, not yet diagnosed). 69 failed. Every failure falls into one of these classes:

| Class | Approx. count | Examples | Root |
|---|---|---|---|
| L1 Nothing staged beside the exe | ~30 | `data/gltf/single.glb` missing; `automation-exclusions.json was not found`; Playwright corpus `0 == 12`; `data/input_actions.json`; material templates (`templateText.has_value()`); msdfgen font; `ArcaneServer.exe` not staged; death-fixture/reporter (Windows-only projects) | A5 |
| L2 `.dll`/`.exe` literal names | ~17 | `cannot copy file … [../HotReloadPluginV1/HotReloadPluginV1.dll]`; `"premake5" == "premake5.exe"`; `ResolveNinja … "ninja.exe"`; `lower.find("arcanetests.exe")`; ArcaneServer `plugin: cannot copy source DLL`; arcbuild probe reports slot `Binaries/Fixture.dll` | 394 `.dll"`/`.exe"`/`.lib"` literals across 59 first-party files |
| L3 Windows-only subsystems no-op | ~13 | Diagnostics watchdog/report (`ReportCount() == base+1` → `0 == 1`); PortableStack `0 >= 3`; ForeignModules empty; EditorLock liveness; witness `timedOut` false | E′ |
| L4 No DXC | ~7 | `ShaderCompiler::Initialize` false (ShaderCompilerTest ×4, Severance, ShaderEditorDocument, HostBoot materials) | I6 |
| L5 Windows path semantics in tests | 1–2 | `IdeLaunch::SameSolutionPath(L"D:\\dev\\.\\Game\\Game.slnx", …)` | test data |
| L6 Unclassified | 3 | `EditorInspectorTest.cpp:421` `pos != nullptr`; Logger late-upgrade file-sink content; the SIGSEGV | investigate |

## 5. Dependency order of fixes

1. **Toolchain floor:** GCC 14 / Clang 19 + libstdc++ 14 (J1, J2).
2. **premake:** A1, A2, A6, A7, A4, then A5 (postbuild via a lua helper for the shared-lib/exe file name), then A14 (`$ORIGIN` runpath), A9/A10/A11 (SDL3 + whole-archive imgui + `--no-undefined`), A8 (fp-contract).
3. **Upstream:** Astra I1/I2 and Mosaic I3, then sync. NRI D3 (report upstream; the workaround is already local).
4. **Source fixes for ArcaneCore:** E-kReasonMax, C-strncpy_s, D1, B1. ArcaneCore is green after these.
5. **ArcaneClient:** B2, B3, the E Vulkan WIN32 extension, the H seam (exclusion + stub TU + NriDevice guards), H6 backend default.
6. **Hosts:** B4. **Editor:** C `_wgetenv`, E ShellExecuteW.
7. **Tests:** C `_putenv_s`, the CoreDllTest gate, then L1 and L2 (staging and name helper), then L3/L4.

## 6. Smallest buildable slice (reaches green first)

**Slice 1, proven here:** all 12 ThirdParty libs + ArcaneCore + ArcaneAssetPipeline + arccook + arcbuild + ArcaneServer.
- **What it needs:** premake A1/A2/A6 (A7 is cosmetic) plus 4 tiny source fixes (Diagnostics `kReasonMax`, ModuleTable `strncpy_s`, TcpSocket `<fcntl.h>`, Astra `<limits>`).
- **What it does (GCC 14):** it links, and it runs. arccook cooks ReferenceProject. `arcbuild --help` and `probe` work. `ArcaneServer --help` works.
- **First CI lane:** ArcaneTests links ArcaneClient (SDL3 + NRI), so there is no Core-only test exe yet. Either:
  - add an `ArcaneCoreTests` project (a Core-only subset that doesn't link ArcaneClient), or
  - run ArcaneTests with a Core-tag filter once Slice 2 links. That already works with the proof fixes plus SDL3.

**Slice 2, proven here (GCC 14):** + ArcaneClient + ArcaneRuntime + ArcaneTests + the 4 HotReload plugin `.so`s. It needs H + B2/B3 + SDL3 link. ArcaneRuntime boots headless on Vulkan to `plugin_load`.

**Slice 3:** ArcaneEditor. One TU is left (ShellExecuteW), then windowed Vulkan surfaces.

## 7. Phased plan

- **Phase 0, baseline (≈1–2 d).** Pin GCC 14 / Clang 19. Land the premake fixes (A1–A2, A4–A7, A14, fp-contract). Set up a Linux agent with ≥16 GB RAM. Make `premake5 gmake` and `ninja` generate without a fake `VCPKG_ROOT` (SDL3 source for Linux).
- **Phase 1, Core slice green (≈2–3 d).** The Slice 1 source fixes. Upstream Astra/Mosaic fixes. A Linux-correct postbuild (shared-lib name helper, no `.dll` copies, `$ORIGIN`). A Core-only test lane in CI (Jenkins `linux-1`).
- **Phase 2, Client compiles and links (≈3–5 d).**
  - The D3D12 seam, using the "twin TU" pattern rather than scattered `#if`s.
  - Vulkan as the default backend off-Windows.
  - SDL3 via vcpkg `x64-linux` (or a from-source build).
  - A POSIX `compile-shaders.sh` over Linux DXC, plus a `libdxcompiler.so` runtime load.
  - imgui `--whole-archive`, `-Wl,--no-undefined`, `-fvisibility=hidden` plus visibility on `IMGUI_API`.
  - Resolve the NRI double-static copy.
- **Phase 3, tests green (≈1–2 wk).**
  - A sweep of the 394 `.dll`/`.exe` literals (59 files) into one `Platform::SharedLibraryFileName()`/`ExecutableFileName()` helper.
  - Staging parity, Windows-only test gating, path-semantics tests.
  - Diagnose the `AssetReferencesTest` SIGSEGV.
- **Phase 4, platform services (≈2–4 wk).**
  - Diagnostics on POSIX: signal handlers, a libunwind/backtrace stack walk, core-dump or minidump-equivalent output, hang watchdog.
  - PortableStack; ForeignModules via `dl_iterate_phdr`; EditorLock via `kill(pid,0)`; HostWitness via `posix_spawn` + timeout/kill.
  - Shell open via `xdg-open`/`SDL_OpenURL`; XDG data dirs.
  - Adopt `MOSAIC_PLATFORM_*` in this same sweep (it's the existing hygiene-slot item).
- **Phase 5, Editor + windowed (≈1–2 wk).** The ShellExecuteW twin; Vulkan surface extensions from SDL; a crash-reporter story (an out-of-process reporter without dbgeng); arcbuild Make/Ninja live acceptance on Linux (`scripts/verify-arcbuild-posix.sh` stage 2).

## 8. Open questions

1. **Cross-compiler TypeHash stability.** After I1, GCC and Clang agree with each other. But MSVC spells template names differently (`class std::vector<int,class std::allocator<int> >` vs `std::vector<int>`). If any TypeHash or type name is **persisted** (scenes, cooked artifacts, net replication), Windows-authored content won't match on Linux for templated component types. Is the name/hash persisted anywhere?
2. **FP determinism.** Should gcc/clang get `-ffp-contract=off` workspace-wide? The Manifold2D parity tests and physics goldens assume `/fp:strict`.
3. **Standard library.** Is libstdc++ the only supported one, or must libc++ work too (no `<stacktrace>`, older C++23 coverage)?
4. **SDL3 on Linux.** vcpkg `x64-linux` triplet (consistent with Windows), a vendored build, or the system package (needs Ubuntu 25.04+)?
5. **Visibility policy.** `-fvisibility=hidden` engine-wide (mirrors Windows export semantics and fixes the NRI double-copy interposition), or keep default visibility?
6. **CI target.** A Core-only `ArcaneCoreTests` exe, or tag-filtered ArcaneTests once Slice 2 links?
7. **Vulkan ICD.** WSL's `libvulkan.so.1` created a device under headless mode, but I didn't identify which ICD (dzn or lavapipe). GPU-tagged tests on Linux need a known ICD.
8. **D3D12-centric hooks.** Should `GraphicsBackend` stop defaulting to D3D12 on every platform, and should `CreateNoneForTests` stop reporting D3D12?

## 9. Exact commands

```bash
# environment (WSL2 Ubuntu 24.04, no sudo)
git clone https://github.com/T3mps/Arcane.git ~/Arcane            # HEAD 57af0cab
curl -sSL -o premake.tgz https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-linux.tar.gz
curl -sSL -o ninja.zip https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip
apt-get download g++-14 gcc-14 cpp-14 libstdc++-14-dev libgcc-14-dev gcc-14-base g++-14-x86-64-linux-gnu \
  gcc-14-x86-64-linux-gnu cpp-14-x86-64-linux-gnu libasan8 libtsan2 libubsan1 libhwasan0 liblsan0 ...   # + dpkg -x
apt-get download clang-19 libclang-cpp19 libllvm19 libclang-common-19-dev libclang1-19 llvm-19-linker-tools
# clang19 wrapper: clang++ --gcc-install-dir=$HOME/opt/root/usr/lib/gcc/x86_64-linux-gnu/14
# SDL3 3.2.22 headers -> $VCPKG_ROOT/installed/x64-windows-static-md/include (fake VCPKG_ROOT)
cmake .. -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_UNIX_CONSOLE_BUILD=ON ...  # link probe only

# generate
export VCPKG_ROOT=~/fakevcpkg
premake5 gmake                 # GCC
premake5 gmake --cc=clang      # Clang
premake5 ninja                 # generates; rules hard-code g++

# build, per project, bottom-up, keep-going (prebuild .bat disabled)
make -k -Otarget -j5 -C <ProjectDir> -f Makefile config=debug verbose=1 PREBUILDCMDS= \
     CC=<cc> CXX=<cxx> AR=ar CXXFLAGS="-fdiagnostics-color=never -fmax-errors=0"   # clang: -ferror-limit=0
# link probe for ArcaneClient
make ... -C ArcaneClient LDFLAGS="-Wl,--whole-archive $HOME/sdl3-install/lib/libSDL3.a -Wl,--no-whole-archive"
# tests
cd bin/Debug-linux-x86_64-md/ArcaneTests && SDL_VIDEODRIVER=offscreen ./ArcaneTests "~[gpu]" --rng-seed 1 -r junit -o junit.xml
```

### Passes run

| Pass | Tree | Compilers | Result |
|---|---|---|---|
| 1 | vanilla | GCC 13, Clang 18 | Baseline. imgui/NRI cascade; ArcaneCore had 2 failing TUs; arcbuild compiled on GCC 13 but 9 TUs failed on Clang 18. |
| 2 | first proof fixes | GCC 13, Clang 18 | ThirdParty 12/12; ArcaneCore 100% compiled (link failed on -fPIC); Client 1 failing TU. |
| 3 | + pic, -Wno-4251, TcpSocket, FunctionRef | GCC 14, Clang 19 + libstdc++14 | Identical 4 residual files on both. Core, AssetPipeline, arccook, arcbuild and ArcaneServer link. |
| 4/5 | + NriDevice guard, D3D12 stub, test fixes, Astra TypeName | GCC 14 | Client, Runtime and Tests link; tests run as in §4. |

## Appendix: proof fixes (`git diff`, 19 files, +209/−24, against `57af0cab`)

Apply with `git apply` from the Arcane root. `git diff --check` is clean and the files are LF.

```diff
diff --git a/ArcaneClient/src/Arcane/Render/DeviceCreationVulkan.cpp b/ArcaneClient/src/Arcane/Render/DeviceCreationVulkan.cpp
index 91150c60..0ee9ab8c 100644
--- a/ArcaneClient/src/Arcane/Render/DeviceCreationVulkan.cpp
+++ b/ArcaneClient/src/Arcane/Render/DeviceCreationVulkan.cpp
@@ -50,7 +50,12 @@ namespace Arcane
         // headless tests and windowed swapchains.
         const char* kInstanceExtensions[] = {
             VK_KHR_SURFACE_EXTENSION_NAME,
+#if defined(_WIN32)
             VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
+#endif
+            // LINUX PORT (owed): the X11/Wayland surface extension belongs
+            // here, ideally taken from SDL_Vulkan_GetInstanceExtensions so the
+            // list follows the video driver SDL actually picked.
             // NRI capability contract item 1 (hard): NRI's SwapChainVK::Create
             // calls GetPhysicalDeviceSurfaceFormats2KHR and
             // GetPhysicalDeviceSurfaceCapabilities2KHR through UNGUARDED
diff --git a/ArcaneClient/src/Arcane/Render/DeviceRemovedD3D12Stub.cpp b/ArcaneClient/src/Arcane/Render/DeviceRemovedD3D12Stub.cpp
new file mode 100644
index 00000000..d8432bad
--- /dev/null
+++ b/ArcaneClient/src/Arcane/Render/DeviceRemovedD3D12Stub.cpp
@@ -0,0 +1,18 @@
+// Non-Windows twin of the D3D12-only device-removed hooks that live in
+// DeviceCreationD3D12.cpp (a Windows-target TU, excluded from a Linux build by
+// premake5.lua). NriDiagnostics.cpp still names them in its per-backend
+// switch, so a D3D12-less build must still DEFINE them: a D3D12 device can
+// never exist there, so "never removed" and empty observers are exact, not
+// placeholders. Compiles to nothing on Windows (the real definitions win).
+#if !defined(_WIN32)
+
+#include <Arcane/Render/DeviceRemovedObservers.hpp>
+
+namespace Arcane
+{
+    void ObserveDeviceRemovedD3D12() {}
+    void ResetDeviceRemovedLatchD3D12() {}
+    bool D3D12NativeDeviceRemoved(void*) noexcept { return false; }
+}
+
+#endif
diff --git a/ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp b/ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp
index 784ce330..fbc63950 100644
--- a/ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp
+++ b/ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp
@@ -3,7 +3,13 @@
 // NRI headers MUST stay first in this file.
 #include <NRI.h>
 #include <Extensions/NRIDeviceCreation.h>
+#if defined(_WIN32)
 #include <Extensions/NRIWrapperD3D12.h>
+#endif
+// NRIWrapperVK.h is not self-contained (uses AccelerationStructureBits from
+// NRIRayTracing.h, which only NRIWrapperD3D12.h pulls in) -- explicit here so
+// a D3D12-less (Linux) build does not depend on the include order.
+#include <Extensions/NRIRayTracing.h>
 #include <Extensions/NRIWrapperVK.h>
 
 #include "NriDevice.hpp"
@@ -12,7 +18,14 @@
 
 #include <Arcane/Base/ForeignModules.hpp>   // ForeignModules::Report -- the injected-overlay scan, once, after the native device exists
 #include <Arcane/Base/Log.hpp>
+#if defined(_WIN32)
 #include <Arcane/Render/DeviceCreationD3D12.hpp>
+#else
+// LINUX PORT: D3D12 is a Windows-target backend (its headers need <rpc.h>/
+// <dxgi1_6.h>). An EMPTY definition keeps NativeDeviceOwner::Impl's
+// unique_ptr<D3D12DeviceCreation> destructible; nothing ever allocates one.
+namespace Arcane { struct D3D12DeviceCreation {}; }
+#endif
 #include <Arcane/Render/DeviceCreationVulkan.hpp>
 #include <Arcane/Render/GpuInstrumentation.hpp>   // GpuDeviceLostObserved -- the ONE device-lost latch (~NriDevice's teardown gate)
 #include <Arcane/Render/ShaderConventions.hpp>
@@ -107,8 +120,10 @@ namespace Arcane
     {
         if (m_impl->vulkan)
             DestroyVulkanNativeDevice(*m_impl->vulkan);
+#if defined(_WIN32)
         if (m_impl->d3d12)
             DestroyD3D12NativeDevice(*m_impl->d3d12);
+#endif
     }
 
     std::unique_ptr<NativeDeviceOwner> NativeDeviceOwner::Create(const RenderDeviceDesc& desc)
@@ -136,9 +151,14 @@ namespace Arcane
         }
         else
         {
+#if defined(_WIN32)
             owner->m_impl->d3d12 = std::make_unique<D3D12DeviceCreation>();
             if (!CreateD3D12NativeDevice(desc, *owner->m_impl->d3d12))
                 return nullptr;
+#else
+            ARC_ERROR("Native device creation: the D3D12 backend does not exist on this platform (use Vulkan)");
+            return nullptr;
+#endif
         }
 
         // THE INJECTED-OVERLAY SCAN, here and nowhere else: the native device
@@ -277,6 +297,7 @@ namespace Arcane
 
     std::unique_ptr<NriDevice> NriDevice::WrapD3D12(const D3D12DeviceCreation& creation)
     {
+#if defined(_WIN32)
         if (!creation.device)
         {
             ARC_ERROR("[nri] cannot wrap D3D12: the creation half has no device "
@@ -341,6 +362,11 @@ namespace Arcane
         if (wrapped)
             wrapped->m_d3d12Creation = &creation;
         return wrapped;
+#else
+        (void)creation;
+        ARC_ERROR("[nri] cannot wrap D3D12: not a backend on this platform");
+        return nullptr;
+#endif
     }
 
     std::unique_ptr<NriDevice> NriDevice::Wrap(const NativeDeviceOwner& native)
@@ -511,19 +537,23 @@ namespace Arcane
         // layers raise while NRI's own objects go out is attributed to THIS
         // step and not to whatever ran before it (the DXGI queue only stores;
         // see DeviceCreationD3D12.cpp).
+#if defined(_WIN32)
         if (m_backend == GraphicsBackend::D3D12)
         {
             if (m_d3d12Creation)
                 DrainD3D12DebugMessages(*m_d3d12Creation, "before nriDestroyDevice");
             DrainDxgiDebugMessages("before nriDestroyDevice");
         }
+#endif
         nriDestroyDevice(m_device);
+#if defined(_WIN32)
         if (m_backend == GraphicsBackend::D3D12)
         {
             if (m_d3d12Creation)
                 DrainD3D12DebugMessages(*m_d3d12Creation, "after nriDestroyDevice");
             DrainDxgiDebugMessages("after nriDestroyDevice");
         }
+#endif
         m_device        = nullptr;
         m_graphicsQueue = nullptr;
     }
diff --git a/ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp b/ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp
index 16d8c734..436a317e 100644
--- a/ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp
+++ b/ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp
@@ -651,7 +651,20 @@ namespace Arcane
                                                                 NriDevice& shared,
                                                                 std::uint32_t width,
                                                                 std::uint32_t height,
-                                                                const NodeSet& nodes = {});
+                                                                const NodeSet& nodes);
+        // The "defaulted to none" overload. NOT `const NodeSet& nodes = {}`:
+        // NodeSet is a nested class with default member initializers, and a
+        // default argument naming it is evaluated before NriGraphContext is
+        // complete -- GCC rejects that ("could not convert '{}'", the
+        // long-standing NSDMI-in-nested-class rule); MSVC accepts it. The
+        // inline body is a complete-class context, so this spelling is portable.
+        static std::unique_ptr<NriGraphContext> CreateOffscreen(const HostConfig& config,
+                                                                NriDevice& shared,
+                                                                std::uint32_t width,
+                                                                std::uint32_t height)
+        {
+            return CreateOffscreen(config, shared, width, height, NodeSet{});
+        }
 
         ~NriGraphContext();
 
diff --git a/ArcaneClient/src/Arcane/Render/Nri/nodes/FullscreenNodes.cpp b/ArcaneClient/src/Arcane/Render/Nri/nodes/FullscreenNodes.cpp
index c0476ef5..5f01d804 100644
--- a/ArcaneClient/src/Arcane/Render/Nri/nodes/FullscreenNodes.cpp
+++ b/ArcaneClient/src/Arcane/Render/Nri/nodes/FullscreenNodes.cpp
@@ -1462,9 +1462,11 @@ namespace Arcane
         // writing a barrier for it. The stages are the same three
         // RgUsage::ShaderRead derives, so the exit matches what a sampler in
         // any of them expects.
-        constexpr nri::AccessLayoutStage kOffscreenEntry{
+        // static: the tonemap lambda below binds these to const& (an odr-use),
+        // which a non-static local would need captured -- Clang enforces it.
+        static constexpr nri::AccessLayoutStage kOffscreenEntry{
             nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::ALL };
-        constexpr nri::AccessLayoutStage kOffscreenExit{
+        static constexpr nri::AccessLayoutStage kOffscreenExit{
             nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
             nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER
                 | nri::StageBits::COMPUTE_SHADER };
diff --git a/ArcaneCore/src/Arcane/Base/Diagnostics.cpp b/ArcaneCore/src/Arcane/Base/Diagnostics.cpp
index 0c53876d..ec269ff4 100644
--- a/ArcaneCore/src/Arcane/Base/Diagnostics.cpp
+++ b/ArcaneCore/src/Arcane/Base/Diagnostics.cpp
@@ -210,6 +210,10 @@ namespace
     // fresh process on the paths that matter (the hosts quit on the latch).
     std::atomic<bool> g_gpuDeviceLost{false};
 
+    // Platform-neutral: FormatReason (outside every _WIN32 block) sizes its
+    // per-thread buffer with this, so it cannot live inside the guard below.
+    constexpr std::size_t kReasonMax  = 1024;
+
 #if defined(_WIN32)
     DWORD  g_mainThreadId = 0;
     LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
@@ -223,7 +227,6 @@ namespace
     // ---- crash thread -----------------------------------------------------
 
     constexpr std::size_t kPathMax    = 1024;   // UTF-8 bytes, generous vs MAX_PATH
-    constexpr std::size_t kReasonMax  = 1024;
     constexpr std::size_t kMaxFrames  = 96;
     constexpr std::size_t kSectionRsv = 32 * 1024;   // the walked thread's text
     constexpr std::size_t kHeaderRsv  = 8 * 1024;    // the .txt header
diff --git a/ArcaneCore/src/Arcane/Base/ModuleTable.cpp b/ArcaneCore/src/Arcane/Base/ModuleTable.cpp
index 30be1b7d..7297a5db 100644
--- a/ArcaneCore/src/Arcane/Base/ModuleTable.cpp
+++ b/ArcaneCore/src/Arcane/Base/ModuleTable.cpp
@@ -7,6 +7,7 @@
 
 #include <Arcane/Base/ModuleTable.hpp>
 
+#include <algorithm>
 #include <atomic>
 #include <cstring>
 #include <mutex>
@@ -54,7 +55,11 @@ namespace Arcane::Diagnostics
             ModuleEntry& e = g_tables[inactive][i];
             e.base = modules[i].base;
             e.size = modules[i].size;
-            strncpy_s(e.name, modules[i].name.c_str(), _TRUNCATE);
+            // Portable spelling of strncpy_s(..., _TRUNCATE) (MSVC CRT only):
+            // copy at most sizeof-1 bytes and always NUL-terminate.
+            const std::size_t n = std::min(modules[i].name.size(), sizeof(e.name) - 1);
+            std::memcpy(e.name, modules[i].name.data(), n);
+            e.name[n] = '\0';
         }
         g_counts[inactive].store(count, std::memory_order_release);
 
diff --git a/ArcaneCore/src/Arcane/Net/TcpSocket.hpp b/ArcaneCore/src/Arcane/Net/TcpSocket.hpp
index 197c27f7..9e3087c0 100644
--- a/ArcaneCore/src/Arcane/Net/TcpSocket.hpp
+++ b/ArcaneCore/src/Arcane/Net/TcpSocket.hpp
@@ -28,6 +28,7 @@ using SocketType = SOCKET;
 #include <arpa/inet.h>
 #include <netinet/tcp.h>
 #include <unistd.h>
+#include <fcntl.h>      // fcntl/F_GETFL/F_SETFL/O_NONBLOCK (SetNonBlocking); glibc pulls nothing transitively
 #include <cerrno>
 using SocketType = int;
 #define INVALID_SOCK -1
diff --git a/ArcaneCore/src/Arcane/Util/FunctionRef.hpp b/ArcaneCore/src/Arcane/Util/FunctionRef.hpp
index c426417c..d8fe6586 100644
--- a/ArcaneCore/src/Arcane/Util/FunctionRef.hpp
+++ b/ArcaneCore/src/Arcane/Util/FunctionRef.hpp
@@ -20,6 +20,28 @@ namespace Arcane
         void* m_obj = nullptr;
         R (*m_thunk)(void*, Args...) = nullptr;
 
+        // Function TYPES need reinterpret_cast: static_cast between a function
+        // pointer and void* is an MSVC extension that GCC and Clang reject
+        // ("invalid static_cast"). The round trip through void* is
+        // conditionally-supported and works on every target Arcane builds for.
+        template <class T>
+        static void* Erase(T& f) noexcept
+        {
+            if constexpr (std::is_function_v<T>)
+                return reinterpret_cast<void*>(&f);
+            else
+                return const_cast<void*>(static_cast<const void*>(std::addressof(f)));
+        }
+
+        template <class T>
+        static T& Restore(void* o) noexcept
+        {
+            if constexpr (std::is_function_v<T>)
+                return *reinterpret_cast<T*>(o);
+            else
+                return *static_cast<T*>(o);
+        }
+
     public:
         FunctionRef() = default;
 
@@ -32,9 +54,9 @@ namespace Arcane
             // yields the function's stable address (not a temporary) -- safe. Same for
             // any lvalue callable. Do NOT bind a function-POINTER rvalue (e.g. a cast
             // result): F would deduce as a pointer type and m_obj would dangle.
-            : m_obj(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
+            : m_obj(Erase<std::remove_reference_t<F>>(f)),
               m_thunk(+[](void* o, Args... a) -> R {
-                  return (*static_cast<std::remove_reference_t<F>*>(o))(static_cast<Args&&>(a)...);
+                  return Restore<std::remove_reference_t<F>>(o)(static_cast<Args&&>(a)...);
               })
         {
         }
diff --git a/ArcaneEditor/src/App/EditorApp.cpp b/ArcaneEditor/src/App/EditorApp.cpp
index 141adfbc..440ada7d 100644
--- a/ArcaneEditor/src/App/EditorApp.cpp
+++ b/ArcaneEditor/src/App/EditorApp.cpp
@@ -1681,8 +1681,16 @@ namespace Arcane::Editor
         // RecentProjects.cpp). With LOCALAPPDATA unset or unwritable, ImGui's
         // exe-dir imgui.ini default stands -- degraded, never broken.
         std::filesystem::path dir;
+#if defined(_WIN32)
         if (const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA"); localAppData && *localAppData)
             dir = std::filesystem::path(localAppData) / L"Arcane" / L"editor" / L"layouts";
+#else
+        // XDG base-directory spec: $XDG_DATA_HOME, else ~/.local/share.
+        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
+            dir = std::filesystem::path(xdg) / "Arcane" / "editor" / "layouts";
+        else if (const char* home = std::getenv("HOME"); home && *home)
+            dir = std::filesystem::path(home) / ".local" / "share" / "Arcane" / "editor" / "layouts";
+#endif
         if (dir.empty())
             return;
         std::error_code ec;
diff --git a/ArcaneEditor/src/main.cpp b/ArcaneEditor/src/main.cpp
index b1326486..6c721d7d 100644
--- a/ArcaneEditor/src/main.cpp
+++ b/ArcaneEditor/src/main.cpp
@@ -39,8 +39,10 @@ static constexpr const wchar_t* kAppUserModelId = L"dev.starworks.arcane";
 // the EXE to redirect device creation into the vendored D3D12Core.dll under
 // .\D3D12\. Version must match the vendored package; the proof it took is NRI
 // logging "Using ID3D12Device10+".
+#if defined(_WIN32)   // Agility SDK: a D3D12 (Windows) loader handshake only
 extern "C" __declspec(dllexport) extern const unsigned D3D12SDKVersion = 619;
 extern "C" __declspec(dllexport) extern const char*    D3D12SDKPath    = ".\\D3D12\\";
+#endif
 
 // ===== THE EDITOR'S FULL PROCESS EXIT-CODE TABLE ============================
 // Gathered in ONE place. This function's own `return rc;` at the bottom is the
diff --git a/ArcaneRuntime/src/main.cpp b/ArcaneRuntime/src/main.cpp
index b476de7f..cf9dcab4 100644
--- a/ArcaneRuntime/src/main.cpp
+++ b/ArcaneRuntime/src/main.cpp
@@ -21,8 +21,10 @@
 // the EXE to redirect device creation into the vendored D3D12Core.dll under
 // .\D3D12\. Version must match the vendored package; NRI logging "Using
 // ID3D12Device10+" is the confirmation that the redirect took.
+#if defined(_WIN32)   // Agility SDK: a D3D12 (Windows) loader handshake only
 extern "C" __declspec(dllexport) extern const unsigned D3D12SDKVersion = 619;
 extern "C" __declspec(dllexport) extern const char*    D3D12SDKPath    = ".\\D3D12\\";
+#endif
 
 namespace
 {
diff --git a/ArcaneTests/src/MeshNodeTest.cpp b/ArcaneTests/src/MeshNodeTest.cpp
index d8c71091..381ff249 100644
--- a/ArcaneTests/src/MeshNodeTest.cpp
+++ b/ArcaneTests/src/MeshNodeTest.cpp
@@ -329,12 +329,22 @@ namespace
                 m_hadOld = true;
                 m_old = old;
             }
+#if defined(_WIN32)
             _putenv_s(name, value.c_str());
+#else
+            ::setenv(name, value.c_str(), 1);
+#endif
         }
 
         ~ScopedEnvironmentValue()
         {
+#if defined(_WIN32)
             _putenv_s(m_name, m_hadOld ? m_old.c_str() : "");
+#else
+            // _putenv_s(name, "") REMOVES the variable; POSIX spells that unsetenv.
+            if (m_hadOld) ::setenv(m_name, m_old.c_str(), 1);
+            else          ::unsetenv(m_name);
+#endif
         }
 
     private:
diff --git a/ArcaneTests/src/test_main.cpp b/ArcaneTests/src/test_main.cpp
index 8a2773f8..4b76bed6 100644
--- a/ArcaneTests/src/test_main.cpp
+++ b/ArcaneTests/src/test_main.cpp
@@ -13,8 +13,10 @@
 // the EXE to redirect device creation into the vendored D3D12Core.dll under
 // .\D3D12\. Version must match the vendored package; the proof it took is NRI
 // logging "Using ID3D12Device10+".
+#if defined(_WIN32)   // Agility SDK: a D3D12 (Windows) loader handshake only
 extern "C" __declspec(dllexport) extern const unsigned D3D12SDKVersion = 619;
 extern "C" __declspec(dllexport) extern const char*    D3D12SDKPath    = ".\\D3D12\\";
+#endif
 
 int main(int argc, char* argv[]) {
     // Install the shared context in the TEST module BEFORE any test computes a
diff --git a/ThirdParty/Astra/include/Astra/Container/SmallVector.hpp b/ThirdParty/Astra/include/Astra/Container/SmallVector.hpp
index 003e393f..ca8a41cd 100644
--- a/ThirdParty/Astra/include/Astra/Container/SmallVector.hpp
+++ b/ThirdParty/Astra/include/Astra/Container/SmallVector.hpp
@@ -4,6 +4,7 @@
 #include <cstring>
 #include <initializer_list>
 #include <iterator>
+#include <limits>
 #include <memory>
 #include <new>
 #include <type_traits>
diff --git a/ThirdParty/Astra/include/Astra/Core/TypeID.hpp b/ThirdParty/Astra/include/Astra/Core/TypeID.hpp
index 81b1577b..db31df10 100644
--- a/ThirdParty/Astra/include/Astra/Core/TypeID.hpp
+++ b/ThirdParty/Astra/include/Astra/Core/TypeID.hpp
@@ -187,6 +187,15 @@ namespace Astra
             if (end == std::string_view::npos || end <= start)
                 return "Unknown";
 
+            #if defined(ASTRA_COMPILER_GCC)
+                // GCC lists the function's OTHER bindings after T, ';'-separated:
+                // "[with T = Arcane::Transform; std::string_view = std::basic_string_view<char>]".
+                // Without this cut every GCC type name (and so every TypeHash) carries
+                // that tail, and name-keyed lookups ("Arcane::Transform") never match.
+                if (const size_t semi = funcName.find(';', start); semi != std::string_view::npos && semi < end)
+                    end = semi;
+            #endif
+
             // Extract the type name
             std::string_view typeName = funcName.substr(start, end - start);
 
diff --git a/ThirdParty/NRI/premake5.lua b/ThirdParty/NRI/premake5.lua
index 3d31ed73..a8a8b5e5 100644
--- a/ThirdParty/NRI/premake5.lua
+++ b/ThirdParty/NRI/premake5.lua
@@ -66,9 +66,6 @@ project "NRI"
         "Source/Shared/**.h",
         "Source/Shared/**.hpp",
         "Source/Shared/**.cpp",
-        "Source/D3D12/**.h",
-        "Source/D3D12/**.hpp",
-        "Source/D3D12/**.cpp",
         "Source/VK/**.h",
         "Source/VK/**.hpp",
         "Source/VK/**.cpp",
@@ -97,7 +94,6 @@ project "NRI"
     defines {
         -- Backends: D3D12 + VK + NONE + Validation. D3D11/WGPU/NVAPI/AMDAGS/
         -- NVTX left undefined (see the file-header comment above).
-        "NRI_ENABLE_D3D12_SUPPORT=1",
         "NRI_ENABLE_VK_SUPPORT=1",
         "NRI_ENABLE_NONE_SUPPORT=1",
         "NRI_ENABLE_VALIDATION_SUPPORT=1",
@@ -112,7 +108,6 @@ project "NRI"
         -- is a CMake-only symbol that generates Include/NRIAgilitySDK.h (a
         -- file this premake build never produces); it appears nowhere in
         -- ThirdParty/NRI/Source or ThirdParty/NRI/Include.
-        "NRI_ENABLE_AGILITY_SDK_SUPPORT=1",
         -- NRI_Shared's PUBLIC compile definitions (CMakeLists.txt:417-423).
         "WIN32_LEAN_AND_MEAN",
         "NOMINMAX",
@@ -136,6 +131,15 @@ project "NRI"
         buildoptions { "/bigobj" }
         fatalwarnings { "All" }
         defines { "VK_USE_PLATFORM_WIN32_KHR" }   -- VK backend (CMakeLists.txt:749-751)
+        -- D3D12 (+ the Agility SDK path, see the comment above) is a Windows-
+        -- target backend: its headers pull <rpc.h>/<dxgi1_6.h>, which no
+        -- Linux sysroot has. Vulkan, NONE and Validation stay on every target.
+        defines { "NRI_ENABLE_D3D12_SUPPORT=1", "NRI_ENABLE_AGILITY_SDK_SUPPORT=1" }
+        files {
+            "Source/D3D12/**.h",
+            "Source/D3D12/**.hpp",
+            "Source/D3D12/**.cpp",
+        }
 
     filter "configurations:Debug"
         runtime "Debug"
diff --git a/build/arcane.lua b/build/arcane.lua
index d55057db..5bad0dc0 100644
--- a/build/arcane.lua
+++ b/build/arcane.lua
@@ -207,7 +207,8 @@ function arcane_game_module(name)
 
         defines {
             "GAME_BUILD_DLL",                         -- kept for an external module's own GAME_API; ARCANE_GAME_MODULE needs no define
-            "IMGUI_API=__declspec(dllimport)",        -- adopt ArcaneClient.dll's single GImGui
+            -- adopt ArcaneClient.dll's single GImGui (ELF: no import decoration exists)
+            "IMGUI_API=" .. (os.target() == "windows" and "__declspec(dllimport)" or ""),
             "_CRT_SECURE_NO_WARNINGS",
             "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
         }
diff --git a/premake5.lua b/premake5.lua
index 259cec74..ea46d003 100644
--- a/premake5.lua
+++ b/premake5.lua
@@ -14,6 +14,21 @@ if not VCPKG_ROOT then
 end
 VCPKG_INSTALLED_MD = VCPKG_ROOT .. "/installed/x64-windows-static-md"
 
+-- imgui's IMGUI_API spelling per target. __declspec is a PE/COFF-only
+-- keyword: GCC rejects it outright on ELF and Clang needs -fdeclspec, so a
+-- Linux generation must not see it. ELF exports every default-visibility
+-- symbol (no import decoration exists), so the empty spelling is correct
+-- there until the workspace adopts -fvisibility=hidden -- at which point this
+-- becomes __attribute__((visibility("default"))), best set from an
+-- IMGUI_USER_CONFIG header rather than a command-line define.
+if os.target() == "windows" then
+    ARCANE_IMGUI_EXPORT = "__declspec(dllexport)"
+    ARCANE_IMGUI_IMPORT = "__declspec(dllimport)"
+else
+    ARCANE_IMGUI_EXPORT = ""
+    ARCANE_IMGUI_IMPORT = ""
+end
+
 workspace "Arcane"
     architecture "x64"
     startproject "ArcaneTests"
@@ -26,6 +41,13 @@ workspace "Arcane"
         buildoptions { "/utf-8", "/arch:AVX2" }   -- AVX2 is the x86 min-spec for the engine (Arcane::Simd)
     filter { "system:linux or system:macosx", "architecture:x86_64" }
         buildoptions { "-mavx2", "-mfma" }         -- gcc/clang x64 parity; ARM port supplies NEON flags later
+    -- ELF: every static lib that links INTO a shared object (enkiTS +
+    -- Manifold2D into libArcaneCore.so, NRI/imgui/freetype/msdfgen into
+    -- libArcaneClient.so) must be position-independent, or ld refuses the
+    -- relocation (R_X86_64_TPOFF32 against enkiTS's thread_local, first).
+    -- PE/COFF has no such rule, which is why Windows never needed it.
+    filter "system:not windows"
+        pic "On"
     filter {}
 
     -- C4251 ("needs to have dll-interface"): disabled workspace-wide, deliberately.
@@ -36,7 +58,11 @@ workspace "Arcane"
     -- structurally impossible here. The "real" fixes (pimpl-everything, function-
     -- only exports) buy nothing under that contract; Unreal ships with 4251
     -- disabled engine-wide for the same reason.
-    disablewarnings { "4251" }
+    -- MSVC-numbered, so MSVC-only: premake passes it through verbatim to
+    -- GCC/Clang as -Wno-4251 (Clang: -Wunknown-warning-option on every TU).
+    filter "system:windows"
+        disablewarnings { "4251" }
+    filter {}
 
     -- "-md" suffix keeps ThirdParty wrapper outputs (each dep builds into
     -- bin/ under its own dir) separate from the static-CRT flavors the
@@ -55,10 +81,10 @@ workspace "Arcane"
     -- the imgui static lib builds with dllexport, the DLL's own TUs match,
     -- and consumers (ArcaneTests/ArcaneRuntime) import. Without this each module
     -- keeps its own null GImGui and ShowDemoWindow() from the test exe asserts.
-    THIRDPARTY_IMGUI_API        = "__declspec(dllexport)"
+    THIRDPARTY_IMGUI_API        = ARCANE_IMGUI_EXPORT
     -- imgui-node-editor links into ArcaneEditor.exe, which IMPORTS imgui from
     -- ArcaneClient.dll -- its ImGui calls must be dllimport (see the wrapper).
-    THIRDPARTY_NODE_EDITOR_IMGUI_API = "__declspec(dllimport)"
+    THIRDPARTY_NODE_EDITOR_IMGUI_API = ARCANE_IMGUI_IMPORT
 
     IncludeDir = {}
     IncludeDir["ArcaneCore"]       = "%{wks.location}/ArcaneCore/src"
@@ -674,9 +700,23 @@ project "ArcaneClient"
 
     -- Shaders are data: compiled at build time, loaded by name at runtime,
     -- hot-reloadable. The script is the single swap point for ShaderMake.
-    prebuildcommands {
-        'call "%{wks.location}/data/shaders/compile-shaders.bat"',
-    }
+    -- Windows only: compile-shaders.bat drives the vendored dxc.exe. A Linux
+    -- generation needs a POSIX twin over the Linux dxc release (same flags);
+    -- until then the prebuild must not run `call` under /bin/sh.
+    filter "system:windows"
+        prebuildcommands {
+            'call "%{wks.location}/data/shaders/compile-shaders.bat"',
+        }
+    -- The two D3D12-only TUs (device creation + DRED crash backend) include
+    -- <d3d12.h>/<dxgi1_6.h> unconditionally; they are not part of a
+    -- Vulkan-only (Linux) build. Their callers still need #if guards / a
+    -- Vulkan-only stub before ArcaneClient LINKS on Linux.
+    filter "system:not windows"
+        removefiles {
+            "%{prj.location}/src/Arcane/Render/DeviceCreationD3D12.cpp",
+            "%{prj.location}/src/Arcane/Render/GpuCrashD3D12.cpp",
+        }
+    filter {}
 
     includedirs {
         "%{prj.location}/src",
@@ -723,7 +763,7 @@ project "ArcaneClient"
         -- This DLL's own TUs include imgui.h; they must export the same
         -- IMGUI_API as the imgui static lib's object files (set via the
         -- THIRDPARTY_IMGUI_API workspace global). IMGUI_IMPL_API follows.
-        "IMGUI_API=__declspec(dllexport)",
+        "IMGUI_API=" .. ARCANE_IMGUI_EXPORT,
     }
 
     filter "system:windows"
@@ -917,7 +957,7 @@ project "ArcaneRuntime"
     if os.target() == "windows" then
         dependson { "ArcaneCrashReporter" }   -- staged by the postbuild below; emitted for a Windows target only
     end
-    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=__declspec(dllimport)" }
+    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=" .. ARCANE_IMGUI_IMPORT }
     postbuildcommands {
         -- F2b Task 5: cook FIRST, then stage the cooked artifacts -- cook-then-copy per
         -- stager, because under /m no postbuild orders against another project's
@@ -1062,7 +1102,7 @@ project "ArcaneEditor"
     if os.target() == "windows" then
         dependson { "ArcaneCrashReporter" }   -- staged by the postbuild below; emitted for a Windows target only
     end
-    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=__declspec(dllimport)" }
+    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=" .. ARCANE_IMGUI_IMPORT }
     postbuildcommands {
         -- F2b Task 5: cook FIRST, then stage the cooked artifacts -- same cook-then-copy
         -- shape as ArcaneRuntime's matching comment above (safe under /m: idempotent by
@@ -1659,6 +1699,13 @@ project "ArcaneTests"
     -- their normal per-config behaviour.
     defines { "MOSAIC_ENABLE_ASSERTS" }
 
+    -- CoreDllTest.cpp answers "which module owns this address" with
+    -- GetModuleHandleEx(FROM_ADDRESS) -- a PE question. Its ELF twin is
+    -- dladdr(); until that twin exists the case is Windows-target only.
+    filter "system:not windows"
+        removefiles { "%{prj.location}/src/CoreDllTest.cpp" }
+    filter {}
+
     -- arccook (F2b Task 5) must exist before this project's postbuild runs it.
     -- arcbuild-process-fixture (Task 5, multibackend hardening) must exist
     -- before the [build] process-fixture integration tests can spawn it --
@@ -1772,7 +1819,7 @@ project "ArcaneTests"
         "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
         -- imgui is exported from ArcaneClient.dll; this exe imports it (matches the
         -- DLL's dllexport so <imgui.h> here resolves to the DLL's symbols).
-        "IMGUI_API=__declspec(dllimport)",
+        "IMGUI_API=" .. ARCANE_IMGUI_IMPORT,
     }
 
     filter "system:windows"
@@ -1843,7 +1890,7 @@ local function test_plugin(name, defs)
         -- IMGUI_API=dllimport: adopt ArcaneClient.dll's single GImGui, exactly as
         -- arcane.lua does for a real module.
         defines (defs)
-        defines { "IMGUI_API=__declspec(dllimport)" }
+        defines { "IMGUI_API=" .. ARCANE_IMGUI_IMPORT }
         filter "system:windows"
             systemversion "latest"
             buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj" }   -- /utf-8: spdlog/fmt via Log.hpp, as arcane.lua sets
```

The SDL3 link line for the Linux link probe was passed on the make command line, not committed to premake. Its proper premake form, a `filter "system:linux"` with `links { "SDL3" }` plus a Linux SDL3 source, is still owed and depends on open question 4.