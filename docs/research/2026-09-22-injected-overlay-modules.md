# Injected overlay / hook modules known to destabilise D3D12 and Vulkan hosts

Research note, 2026-09-22, for a startup detection in the Arcane hosts (a one-line `ARC_WARN`, a verify-report field and a non-modal editor notice -- never a message box, so headless/CI runs are unaffected). Companion to the editor close-crash fix on `fix/editor-shutdown-dxgi-break`, whose root cause was one of these modules over-releasing our `ID3D12Device`.

**UE 5.8.2 checked first:** the engine source carries NO known-bad-module list (nothing in `Runtime/Core/Private/Windows`, the D3D11/D3D12/Vulkan RHIs, or `Programs/CrashReportClient` names Nahimic, RTSS, GPU Tweak or any overlay). Epic's attribution lives in their crash backend, not the engine. So the table below comes from maintained third-party blocklists and this desk's own evidence.

## Tier 1 -- proven to corrupt or crash the host (detect and warn)

| Module (case-insensitive) | Product | What it does to us | Evidence |
|---|---|---|---|
| `GTIII-OSD64.dll`, `GTIII-OSD64-GL.dll`, `GTIII-OSD64-VK.dll` | ASUS GPU Tweak III on-screen display (`GTIII-OSD64.exe`, `GTIII-OSDCtrl.exe`) | Hooks the D3D12/DXGI (and GL/VK) present path; releases the app's `ID3D12Device` one time too many every 31 presented frames, so the device dies part-way through teardown and the debug layer's CORRUPTION break kills the process at close. Also injects its own D3D12 ERRORs when it uploads its overlay texture. | This desk, 2026-09-22 (vtable-hook trace, see the fix commit); ROG forum "GPU Tweak III OSD cause game crash on exit" and "GTIII_OSD64.dll causing game and driver crashes when using OSD with DLSS/Reflex" (crash dumps point into the DLL at exit); RuneLite launcher blocklist; Sodium (Minecraft) issue #2637 (texture corruption + log spam). |
| `NahimicOSD.dll`, `NahimicMSIOSD.dll`, `Nahimic2OSD.dll`, `Nahimic2DevProps.dll`, `NHAsusStrixOSD.dll`, `AudioDevProps2.dll` | Nahimic (A-Volute) audio suite bundled by MSI/ASUS/others -- its OSD injects into every 3D process | Present-path hook; long history of crashes in D3D11/D3D12 apps and in browser GPU processes. `AudioDevProps2.dll` is the same family's device-property module. | Firefox DLL blocklist (bugs 1233556, 1360029, 1540883, 1804023 "causes crashes in the GPU process", 1269244); RuneLite blocklist; loaded in our editor on this desk (`NahimicOSD.dll`, `AudioDevProps2.dll`). |
| `SS2OSD.dll`, `SS2DevProps.dll`, `SS3DevProps.dll` | ASUS Sonic Studio 2 / 3 (A-Volute lineage) | Same OSD hook family as Nahimic; documented game crashes at launch and in D3D11 tests. | Firefox blocklist (bugs 1262348, 1540883); RuneLite blocklist; PassMark and ROG forum reports. |
| `RTSSHooks.dll`, `RTSSHooks64.dll` | RivaTuner Statistics Server (ships with MSI Afterburner, EVGA Precision X1, others) | Present/swapchain hook and frame limiter; widely documented D3D12 launch failures and crashes; several games refuse to start with it. | RuneLite blocklist; many publisher support pages. |
| `k_fps32.dll`, `k_fps64.dll` | Razer Cortex in-game FPS overlay | Present hook; historically crash-prone (Mod Organizer 2 still warns; may be fixed in recent versions). | RuneLite blocklist; ModOrganizer2 issue #1325. |

## Tier 2 -- hooks the present path; usually benign, but attribution matters (log at INFO, tag debug messages)

| Module | Product |
|---|---|
| `nvspcap64.dll` | NVIDIA ShadowPlay / GeForce Experience / NVIDIA app in-game overlay (loaded in our editor on this desk) |
| `NvCamera64.dll` | NVIDIA Ansel |
| `GameOverlayRenderer64.dll` | Steam overlay |
| `DiscordHook64.dll` | Discord overlay |
| `graphics-hook64.dll` | OBS Studio game capture |
| `ow-graphics-hook64.dll` | Overwolf |
| `EOSOVH-Win64-Shipping.dll` | Epic Online Services overlay |
| `igo64.dll` | EA app / Origin in-game overlay |
| `TwitchNativeOverlay64.dll` | Twitch Studio overlay |
| `medal-hook64.dll` | Medal |
| `fraps64.dll`, `bdcap64.dll` | Fraps, Bandicam |
| `ReShade64.dll` (or a proxy `dxgi.dll` / `d3d12.dll` beside the exe), `SpecialK64.dll` | ReShade, Special K post-processing injectors |

AMD's Radeon overlay and Xbox Game Bar hook through driver/OS paths rather than a stable injected DLL name and are deliberately not listed; detect them, if ever needed, by vendor API rather than module name.

## Where it shipped

`ArcaneCore/src/Arcane/Base/ForeignModules.hpp` carries the table (`Table()`), the pure matcher (`Classify` / `MatchAll` / `Tier1Names`), the enumeration (`EnumerateProcessModules`), the remembered scan (`Scan` / `LastScan`) and the once-per-process logger (`Report`). `NativeDeviceOwner::Create` (Render/Nri/NriDevice.cpp) calls `Report()` after either backend's native device exists. Both hosts re-`Scan()` in their report path: the names land in `VerifyReport` as `foreignModules` (schemaVersion 10, `{ module, path, product, tier }`), and a Tier 1 hit is named on the `RenderErrorCount` summary line. `Diagnostics::WriteReport` prints the last scan as the `injected :` header line and copies the base names into the `.arcdiag` envelope's `foreignModules` (additive; format version unchanged). The windowed editor publishes one Problems row per Tier 1 module under `diagnostics:foreign-modules`. Tests: `ArcaneTests/src/ForeignModulesTest.cpp` (`[foreign-modules]`), plus the schema cases in `VerifyReportTest.cpp` and `DiagEnvelopeTest.cpp`.

**The table decorates; it does not gate.** Detection is by ORIGIN, from each module's path: under one of the host's own trees (the exe directory, plus the directory of everything `Module::Load` has loaded -- the game module, plugins; `ForeignModules::NoteOwned`) it is ours, under the Windows directory it is the OS's, and anything else got in from outside and is reported as **Tier 3 (uncatalogued)** with its path -- one `ARC_INFO`, a report row, no warning. The table upgrades a known module to Tier 1/2 with product, consequence and remedy, and outranks the path (a catalogued Tier 1 module is Tier 1 wherever its installer put it; `AudioDevProps2.dll` sits in System32 on this desk). A new overlay therefore still shows up in the report the day it appears; only the remedy text needs the table.

**Desk evidence, 2026-09-22, the 900-frame windowed editor run on Aphelyon:** at device creation the scan found `GTIII-OSD64-VK.dll`, `AudioDevProps2.dll`, `NahimicOSD.dll` (Tier 1) and `nvspcap64.dll` (Tier 2); the shutdown re-scan additionally found `GTIII-OSD64.dll` and `GTIII-OSD64-GL.dll`, which GPU Tweak III loads later than its Vulkan hook -- the reason the hosts scan again when they write their report. The armor audit read 11 foreign releases; the run exited 0 with `RenderErrorCount 0 -> 0`.

**Not done, deliberately (option 2 of the 2026-09-22 discussion):** behavioural attribution -- hooking the device's `AddRef`/`Release` and resolving each caller's return address to a module, which is how `GTIII-OSD64.dll` was identified in the first place. It would name a culprit with no table at all, but it patches a shared COM vtable in a process where the overlays patch the same one (hook-chain ordering, a never-restore rule at teardown, two vtables depending on the debug layer), so it belongs behind an opt-in flag with its own spec. Owed.

## Detection shape (as agreed before the commit)

- After device creation, enumerate the process's modules once (`EnumProcessModulesEx` + `GetModuleBaseNameW`), compare case-insensitively against Tier 1 and Tier 2.
- Tier 1: one `ARC_WARN` per module naming the product, the consequence ("can destroy the D3D12 device at close; its own D3D12 errors will appear in the log") and the remedy ("blacklist the Arcane hosts in that overlay's settings or disable its OSD"); a `foreignModules` array in the verify report and the diagnostics envelope; in the windowed editor a one-time Problems-pane notice. Never a modal, never per frame.
- Tier 2: one `ARC_INFO` line and the same report field, so a red lane on a desk with an overlay is attributable.
- The debug-message drains already tag messages by producer; a present Tier 1 module should be named in the `RenderErrorCount` summary so "our error" and "the overlay's error" are told apart.

## Remedies per product (for the warning text)

- GPU Tweak III: OSD settings carry a **Blacklist** ("exclude applications that do not use OSD"); add `ArcaneEditor.exe`, `ArcaneRuntime.exe`, `ArcaneTests.exe`, or disable the OSD. A per-game toggle is a standing ASUS feature request.
- Nahimic / Sonic Studio: disable the in-game overlay in the app, or stop the service; exiting the tray app alone is not enough (it restarts at boot).
- RTSS: add the host to RTSS's application profile with detection "None", or close RTSS.

## Sources

- RuneLite launcher blocklist as quoted in adoptium/adoptium-support#875: `RTSSHooks.dll, RTSSHooks64.dll, NahimicOSD.dll, NahimicMSIOSD.dll, Nahimic2OSD.dll, Nahimic2DevProps.dll, k_fps32.dll, k_fps64.dll, SS2DevProps.dll, SS2OSD.dll, GTIII-OSD64-GL.dll, GTIII-OSD64-VK.dll, GTIII-OSD64.dll`.
- Firefox `WindowsDllBlocklistDefs.in` (mozilla/gecko-dev, `toolkit/xre/dllservices/mozglue/`): `ss2osd.dll`, `ss2devprops.dll`, `ss3devprops.dll`, `audiodevprops2.dll`, `nahimicosd.dll`, `nahimic2osd.dll`, `nahimicmsiosd.dll`, `nhasusstrixosd.dll`, each with its bug number.
- ASUS ROG forums: "GPU Tweak III OSD cause game crash on exit" (td-p/1080187); "GTIII_OSD64.dll causing game and driver crashes when using OSD with DLSS/Reflex" (td-p/1094198); "Feature Request: Per-Game Overlay Toggle in GPU Tweak III" (td-p/1128082). ASUS FAQ 1048435 (GPU Tweak III introduction: OSD blacklist).
- CaffeineMC/sodium#2637 (GPU Tweak III corruption and log spam).
- This desk: `tasklist /m` on a windowed Arcane editor listed `GTIII-OSD64.dll`, `NahimicOSD.dll`, `AudioDevProps2.dll`, `nvspcap64.dll`; the refcount trace in `.superpowers/editor-shutdown-dxgi/`.
