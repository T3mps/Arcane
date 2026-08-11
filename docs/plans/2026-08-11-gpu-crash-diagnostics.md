# GPU Crash Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Execute `docs/specs/2026-08-11-gpu-crash-diagnostics-design.md` -- first-party GPU crash capture (breadcrumbs, fault collection, gpu-stall watchdog) folded into `Arcane::Diagnostics`, emitted as GUID-carrying `.arcdiag` assets that open from the Asset Browser into a `CrashReportDocument`.

**Architecture:** Pure units first (envelope, breadcrumb ring) with TDD; then the Diagnostics provider seam; then per-backend capture bound to Task 1's verified API facts; then host wiring, registry mount, and the document. The spec's config-policy table is law. `IGpuCrashBackend` ships with exactly one (portable) implementation.

**Tech Stack:** C++23, D3D12 (core WriteBufferImmediate + DRED), Vulkan (`VK_EXT_device_fault`, `VK_AMD_buffer_marker`, both optional), NVRHI (never wrapped), Catch2, ImGui.

## Global Constraints

- Repo: `D:\dev\starworks\Arcane` (repo root = workspace). Commit per task on main; do NOT push until the arc completes.
- Build gate every task: VS18 msbuild (`C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe`) `Arcane.slnx /p:Configuration=Debug /m /v:q /nologo` -- 0 errors 0 warnings; Tasks 6, 10, 11 also build Release. Suite runs FROM `bin\Debug-windows-x86_64-md\ArcaneTests\`: `ArcaneTests.exe "~[gpu]"`; capture the seed banner. New files -> rerun `GenerateProjects.bat` (`$env:_APH_NOPAUSE='1'`).
- NEVER run ArcaneEditor.exe/ArcaneRuntime.exe (GPU is desk-only on this machine). The desk battery is the USER's.
- NVRHI boundary rules: never wrap ICommandList, no own barriers; native access ONLY via `getNativeObject()` at the seams this plan names.
- Config policy (spec, locked): pass-level breadcrumbs ON in Debug/Release/Dist; draw-level markers Debug/Release behind a runtime toggle default OFF, compiled out of Dist (`ARCANE_DIST`); DRED full in Debug/Release, lightweight in Dist; all Vulkan extensions optional -- absence degrades, never fails device creation; the report records which layers were active.
- Tests tag `[diag]` (pure units) / `[editor]` (editor-side); never a bare `Arcane::Runtime rt;` -- use the SharedTypeContext harness. `#include <Json.hpp>`, never nlohmann/json.hpp. ASCII comments, UTF-8 no BOM. MSVC output is truth; clangd diagnostics are known noise.
- Editor includes are src-root-relative (`"App/..."`, `"Documents/..."`); engine includes `<Arcane/...>`.
- The working tree may carry user WIP: `git status` first; never `git add -A`; stage exact paths.
- Task 1's committed facts file is REQUIRED READING for Tasks 5-7; where this plan writes `F-n`, the implementer binds the code to that fact rather than guessing.

---

### Task 1: Seam-facts survey (read-only; the plan's verification debt)

**Files:**
- Create: `docs/plans/2026-08-11-gpu-crash-seam-facts.md`

**Interfaces:**
- Produces: numbered facts F-1..F-8 with file:line / doc citations. Tasks 5-7 bind to them.

- [ ] **Step 1: Check reference availability.** `Test-Path D:\dev\starworks\Arcane\.example` -- if the UE tree is present, cite it for F-1/F-2/F-5; if absent, record that and cite Microsoft docs + github.com/NVIDIA/nsight-aftermath-samples instead (WebFetch/WebSearch allowed for this task only).
- [ ] **Step 2: Pin the facts.** Each gets a heading, the answer, and the citation:
  - **F-1** D3D12 marker-buffer recipe: exact heap type/flags for a buffer whose contents remain CPU-readable after device removal, and the `ID3D12GraphicsCommandList2::WriteBufferImmediate` mode semantics (`MARKER_IN` vs `OUT` vs `DEFAULT`) to use for begin/end scope markers.
  - **F-2** DRED enablement: exact interface/enum for full DRED (`ID3D12DeviceRemovedExtendedDataSettings`, auto-breadcrumbs + page fault) AND whether a lightweight tier (`D3D12_DRED_ENABLEMENT`-adjacent) exists in THIS machine's Windows SDK -- cite the header found under the VS18 install. If lightweight is unavailable, Dist falls back to breadcrumbs-only and the spec's table gets a noted amendment.
  - **F-3** Where device-removed becomes observable in Arcane: read `ArcaneClient/src/Arcane/Render/NvrhiMessageCallback.{hpp,cpp}` and `Device.cpp` -- the callback's error path, and where a failed present/submit HRESULT (`DXGI_ERROR_DEVICE_REMOVED`) or `VK_ERROR_DEVICE_LOST` first surfaces. Name the function(s) Task 5/6 hook.
  - **F-4** How the D3D12/Vulkan native device + queues are reachable from engine code (`getNativeObject` types NVRHI exposes for device, command list, queue) -- cite `ThirdParty/nvrhi/include/nvrhi/nvrhi.h` object-type enums.
  - **F-5** Vulkan enablement path: where instance/device extension lists are assembled in `Device.cpp`/DeviceManager, so `VK_EXT_device_fault` + `VK_AMD_buffer_marker` can be requested-if-available; how availability is queried there today.
  - **F-6** `Diagnostics` internals: where `WriteReport` composes the `.txt` and mints sibling paths (`ArcaneClient/src/Arcane/Base/Diagnostics.cpp`), and where hosts set `Config::dumpDir` today (grep `dumpDir` across ArcaneEditor/ArcaneRuntime).
  - **F-7** Registry scan + incremental-add: where `ScanContent` classifies native assets by extension and reads embedded GUIDs, and the exact registration call the material-creation flow uses for a single new asset (grep `CreateMaterialAt` -> registry call).
  - **F-8** The frame's pass seams: the function names in the render path where Canvas passes / tonemap / ImGui / resolver passes begin and end (read `EditorAppFrame.cpp`'s render phases + `ArcaneClient` Host render path) -- the list Task 7 instruments.
- [ ] **Step 3: Commit** `docs: GPU-crash seam facts F-1..F-8 (plan verification)`

### Task 2: `.arcdiag` envelope (pure unit, TDD)

**Files:**
- Create: `ArcaneClient/src/Arcane/Base/DiagEnvelope.hpp`, `DiagEnvelope.cpp`
- Test: `ArcaneTests/src/DiagEnvelopeTest.cpp`

**Interfaces:**
- Produces: `namespace Arcane::Diag` -- `struct Envelope { std::uint32_t formatVersion = 1; Guid guid; std::string kind, timestampUtc, appName, phase, buildInfo; std::string cpuThreadSummary; struct Queue { std::string name, lastCompleted; std::vector<std::string> inFlight; }; std::vector<Queue> queues; struct Fault { std::string type, address, resource; } fault; std::string siblingTxt, siblingDmp, siblingGpuDump; std::vector<std::string> activeLayers; };` and `std::string Serialize(const Envelope&)`, `std::optional<Envelope> Parse(std::string_view json)`, `bool WriteFile(const Envelope&, const std::filesystem::path&)`, `std::optional<Envelope> ReadFile(const std::filesystem::path&)`.

- [ ] **Step 1: Write the failing test** -- round-trip every field; `guid` minted by the caller survives; `Parse` rejects a missing/invalid `formatVersion` or nil guid with nullopt; unknown extra keys are ignored (forward compat):

```cpp
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Guid.hpp>

TEST_CASE("arcdiag envelope round-trips with its guid", "[diag]")
{
    Arcane::Diag::Envelope e;
    e.guid = Arcane::Guid::Generate();
    e.kind = "gpu-crash"; e.phase = "editor-frame"; e.appName = "DiagTest";
    e.queues.push_back({ "direct", "pass:tonemap", { "pass:imgui" } });
    e.fault = { "page-fault", "0xDEADBEEF0000", "ParticleIndices" };
    e.siblingTxt = "r.txt"; e.siblingDmp = "r.dmp";
    e.activeLayers = { "breadcrumbs:pass", "dred:full" };

    const auto back = Arcane::Diag::Parse(Arcane::Diag::Serialize(e));
    REQUIRE(back.has_value());
    CHECK(back->guid == e.guid);
    CHECK(back->kind == "gpu-crash");
    REQUIRE(back->queues.size() == 1);
    CHECK(back->queues[0].lastCompleted == "pass:tonemap");
    CHECK(back->fault.resource == "ParticleIndices");
    CHECK(back->activeLayers.size() == 2);
}

TEST_CASE("arcdiag parse rejects nil guid and bad version", "[diag]")
{
    CHECK_FALSE(Arcane::Diag::Parse("{}").has_value());
    CHECK_FALSE(Arcane::Diag::Parse("{\"formatVersion\":1,\"guid\":\"00000000-0000-0000-0000-000000000000\"}").has_value());
}
```

- [ ] **Step 2: Regen, build -- FAIL (header missing).** Record RED.
- [ ] **Step 3: Implement** with `<Json.hpp>`; `Serialize` writes every field, `Parse` tolerates unknown keys, `WriteFile` writes UTF-8 no BOM. Follow the JSON style of an existing `.arc*` writer (F-7's material writer is the model).
- [ ] **Step 4: Build; run `ArcaneTests.exe "[diag]"` -- PASS; then full `~[gpu]`.**
- [ ] **Step 5: Commit** `feat(diag): .arcdiag envelope -- versioned, GUID-carrying, round-trip tested`

### Task 3: Breadcrumb ring + backend seam (pure unit, TDD)

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/GpuBreadcrumbs.hpp`, `GpuBreadcrumbs.cpp`, `ArcaneClient/src/Arcane/Render/IGpuCrashBackend.hpp`
- Test: `ArcaneTests/src/GpuBreadcrumbsTest.cpp`

**Interfaces:**
- Produces: `Arcane::GpuBreadcrumbs` -- `std::uint32_t BeginScope(std::string_view name)` / `void EndScope(std::uint32_t token)` maintain a bounded ring (capacity 256 scopes/queue, oldest evicted) of `{ id, name, depth }`; `void OnMarkerWritten(std::uint32_t id, bool begin)` is what a backend reports; `struct Snapshot { std::string lastCompleted; std::vector<std::string> inFlight; }` via `Snapshot(...) const` derives, from the highest begin-marker id vs highest end-marker id, which named scopes were in flight. Pure: no GPU calls. Also `IGpuCrashBackend` -- `virtual bool WriteMarker(nvrhi::ICommandList*, std::uint32_t id, bool begin) = 0; virtual void CollectFault(Diag::Envelope&) = 0; virtual const char* Name() const = 0;` -- consumed by Tasks 5/6.

- [ ] **Step 1: Failing tests:** begin/end pairing produces empty inFlight + lastCompleted = last ended scope; a begun-not-ended scope appears in inFlight with its ancestors; ring eviction keeps derivation correct past capacity; `Snapshot` with zero markers reports empty strings (never crashes).
- [ ] **Step 2: RED, then implement, then `[diag]` PASS + full suite.** (Same TDD shape as Task 2 -- test file first, regen, build fails on the missing header, implement, green.)
- [ ] **Step 3: Commit** `feat(render): GpuBreadcrumbs ring + IGpuCrashBackend seam (pure, headless-tested)`

### Task 4: Diagnostics GPU-section provider + `.arcdiag` emission

**Files:**
- Modify: `ArcaneClient/src/Arcane/Base/Diagnostics.hpp`, `Diagnostics.cpp` (bind to F-6)
- Test: `ArcaneTests/src/DiagnosticsTest.cpp` (append)

**Interfaces:**
- Consumes: `Diag::Envelope` (Task 2).
- Produces: `Diagnostics::SetGpuSectionProvider(void(*)(Diag::Envelope&, std::string& humanText, const std::filesystem::path& reportStem, void* user), void* user)` + matching `ClearGpuSectionProvider` -- `reportStem` is the sibling-path stem so the provider can write `<stem>.gpudump` itself and record it in `siblingGpuDump`; `WriteReport` now (a) calls the provider (if set) to fill the envelope's gpu fields and append a `=== GPU ===` block to the `.txt`, (b) always writes `<stem>.arcdiag` beside the `.txt`/`.dmp` with a freshly minted guid, kind derived from the reason string (`gpu-crash`/`gpu-stall` when the reason says so, else `crash`/`hang`), sibling paths filled.

- [ ] **Step 1: Failing tests** (append to DiagnosticsTest.cpp, reuse its fixtures): with a fake provider installed, `WriteReport("gpu-crash: test")` produces a `.txt` containing `=== GPU ===` and an `.arcdiag` whose `ReadFile` parse has kind `gpu-crash`, a valid guid, and the provider-written queue data; without a provider, `.arcdiag` still appears with kind `crash` and empty gpu section; provider cleared -> no GPU block. **CPU path pinned explicitly:** every emitted `.arcdiag` (with or without provider) carries a non-empty `cpuThreadSummary` populated from the same thread walk that feeds the `.txt` (assert it names the registered main thread, mirroring the existing "(MAIN)" `.txt` assertion) -- a plain CPU crash/hang report must be fully renderable by CrashReportDocument, not an empty shell.
- [ ] **Step 2: RED (new assertions fail), implement in Diagnostics.cpp at F-6's composition site, GREEN, full suite.**
- [ ] **Step 3: Commit** `feat(diag): GPU-section provider seam + .arcdiag emission from WriteReport`

### Task 5: D3D12 capture (markers + DRED + device-removed hook)

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/GpuCrashD3D12.cpp` (+ decl in `IGpuCrashBackend.hpp`: `std::unique_ptr<IGpuCrashBackend> MakeD3D12CrashBackend(nvrhi::IDevice*)`)
- Modify: `Device.cpp` (bind to F-2/F-3/F-4)

**Interfaces:**
- Consumes: F-1..F-4; `GpuBreadcrumbs`, `IGpuCrashBackend`.
- Produces: the portable D3D12 backend: marker buffer per F-1, `WriteMarker` via `WriteBufferImmediate` on the native list (F-4), DRED enabled at device create per policy tier (F-2, BEFORE `D3D12CreateDevice`), `CollectFault` reads the marker buffer + `GetDeviceRemovedReason` + DRED breadcrumbs/page-fault into the envelope (resource name from DRED's allocation output) AND writes `<stem>.gpudump` -- the tagged-section container (magic `AGPU`, u32 version, `{tag[16], offset, size}` table; sections: raw marker bytes, raw DRED breadcrumb/page-fault serializations) -- ALWAYS for gpu kinds, even on partial collection; the container writer/reader is a small shared helper in `IGpuCrashBackend.hpp` (`Diag::GpuDumpWriter`) with a `[diag]` round-trip test added in this task. Device.cpp registers the backend + installs the Diagnostics provider when the backend is live; F-3's device-removed observation point calls `Diagnostics::WriteReport("gpu-crash: device removed")`.

- [ ] **Step 1: Implement per the facts file.** Every F-n binding gets a `// F-n:` comment at the call. DRED enablement failure or marker-buffer creation failure logs one WARN and disables that layer -- never fatal; `activeLayers` records what engaged.
- [ ] **Step 2: Build Debug + Release 0/0; full suite (no new tests -- GPU-side code has no CI coverage by design; the desk battery covers it).** State that verification honestly in the report.
- [ ] **Step 3: Commit** `feat(render): D3D12 GPU crash backend -- WriteBufferImmediate markers, tiered DRED, device-removed capture`

### Task 6: Vulkan capture (optional extensions, degrade path)

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/GpuCrashVulkan.cpp` (+ `MakeVulkanCrashBackend` decl)
- Modify: `Device.cpp` (bind to F-5)

**Interfaces:** same seam as Task 5. `VK_EXT_device_fault` + `VK_AMD_buffer_marker` requested only if enumerated (F-5); marker path degrades to CPU-side fence-correlated scope recording when buffer_marker is absent (the ring already carries the data; `OnMarkerWritten` is driven from fence-progress polling instead of GPU writes -- coarser, honest in `activeLayers` as `breadcrumbs:fence`); `CollectFault` queries `vkGetDeviceFaultInfoEXT` when present; ALL captured data (scope ring, fault info, vendor blob) goes into `<stem>.gpudump` via the shared `Diag::GpuDumpWriter` (Task 5) -- one section per source, written always for gpu kinds, `siblingGpuDump` recorded.

- [ ] **Step 1: Implement per F-5; Debug + Release 0/0; full suite; commit** `feat(render): Vulkan GPU crash backend -- device_fault + buffer_marker optional, fence-granularity degrade`

### Task 7: GPU-progress watchdog + pass-seam instrumentation

**Files:**
- Modify: `Diagnostics.cpp` (watchdog thread), `Device.cpp` or the host render path (F-8 seams), `data/EngineConfig` schema site for the draw-marker toggle (find the existing config surface the same file family uses)
- Test: `ArcaneTests/src/DiagnosticsTest.cpp` (append -- pure staleness logic only)

**Interfaces:**
- Produces: `Diagnostics::GpuHeartbeat(std::uint64_t fenceValue) noexcept` -- the render path calls it once per completed frame with the frame fence value; the watchdog thread reports `gpu-stall` when the value hasn't advanced for `Config::gpuStallSeconds` (default 8) while CPU heartbeats continue; one report per stall, re-armed on progress (mirror the CPU watchdog's re-arm shape, INCLUDING the poll-not-deadline test style from the 2026-08-11 flake fix). Pass scopes: `BeginScope/EndScope` calls at each F-8 seam, always-on; draw-level markers behind `EngineConfig` toggle `diagnostics.drawMarkers` (default false), `#if !defined(ARCANE_DIST)`.

- [ ] **Step 1: Failing test for the pure staleness rule** (same-value-N-polls -> stall once -> re-arm on change), poll-based waits.
- [ ] **Step 2: Implement; Debug 0/0; `[diag]` + full suite green; commit** `feat(diag): GPU-progress watchdog + pass-seam breadcrumbs wiring`

### Task 8: Host dumpDir retarget

**Files:**
- Modify: the F-6 host sites (`EditorApp.cpp` boot config + the project-open tail `OnProjectOpened`'s call-site family beside `RetargetLayoutIni`; `ArcaneRuntime` equivalent)

**Interfaces:**
- Produces: `Diagnostics::RetargetDumpDir(const std::filesystem::path&)` (Base) -- switches the write location live; editor/runtime call it with `<project>/Saved/Diagnostics` on project open and back to the exe-relative default when converging project-less (the switch failure fallback). The call sits immediately after `RetargetLayoutIni()` at each site -- same retarget family, same comment style.

- [ ] **Step 1: Implement + a `[diag]` test for `RetargetDumpDir` (WriteReport lands in the new dir).** Build Debug 0/0, full suite, commit `feat(diag): per-project Saved/Diagnostics dump dir -- retargeted with the layout ini`

### Task 9: Registry mount + incremental register + Problems notify

**Files:**
- Modify: `ArcaneClient/src/Arcane/Project/AssetRegistry.{hpp,cpp}` + the scan site (F-7), editor report-written path
- Test: `ArcaneTests/src/AssetRegistryTest.cpp` (append)

**Interfaces:**
- Produces: the scan mounts `<project>/Saved/Diagnostics` under the `diag://` scheme when the directory exists; `.arcdiag` classifies as a native asset whose guid is read from the envelope (reuse `Diag::ReadFile`). Editor-side: when a report is written during a session (the Diagnostics publication seam already broadcasts -- F-6 names the hook), register the new file via F-7's single-asset call and `Publish("diagnostics:reports", ...)` one Problems diagnostic ("crash report written -- open from the Assets browser").

- [ ] **Step 1: Failing registry test:** a temp project fixture with `Saved/Diagnostics/x.arcdiag` (written via `Diag::WriteFile`) scans into the registry under `diag://x.arcdiag` with the envelope's guid; no `Saved/Diagnostics` dir -> no mount, no error.
- [ ] **Step 2: RED -> implement -> GREEN + full suite; commit** `feat(project): diag:// mount -- .arcdiag reports are GUID-registered assets`

### Task 10: Asset Browser kind + CrashReportDocument

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetBrowser.hpp` (kind classify + icon), `ArcaneEditor/src/App/EditorApp.cpp` (factory registration beside the `.arcmat` one)
- Create: `ArcaneEditor/src/Documents/CrashReportDocument.hpp`, `.cpp`
- Test: `ArcaneTests/src/CrashReportDocumentTest.cpp`

**Interfaces:**
- Consumes: `DocumentHost::RegisterFactory(".arcdiag", ...)` (existing seam), `Diag::ReadFile`.
- Produces: `Arcane::Editor::CrashReportDocument : EditorDocument` -- read-only, never dirty; loads the envelope, renders: header (kind/time/phase/build), per-queue timeline (lastCompleted then inFlight, in-flight rows highlighted with the theme's warning color), fault block, CPU thread summary, `activeLayers` line, buttons shell-opening `siblingTxt`/`siblingDmp`/`siblingGpuDump` (gpu-dump button only when the field is non-empty; renders the container's section inventory inline) (`ShellExecute` via the existing show-in-explorer helper the browser uses -- F-7's file has it). AssetBrowser: `.arcdiag` -> kind `Diagnostic`, lucide icon `ICON_LC_BUG` (verify in IconsLucide.h; nearest match if absent).

- [ ] **Step 1: Failing tests:** headless doc construction from a `Diag::WriteFile` fixture exposes parsed fields (test the model half, not ImGui); `DocumentHost.OpenPath("x.arcdiag")` routes to the factory (mirror the existing `.arcmat` routing test in EditorDocumentHostTest.cpp); browser classification test for the new kind (mirror AssetBrowserTest's kind cases).
- [ ] **Step 2: RED -> implement -> GREEN; Debug + Release 0/0; full suite; commit** `feat(editor): CrashReportDocument -- .arcdiag opens from the Assets browser into the document area`

### Task 11: Dev fault command + closeout

**Files:**
- Modify: the editor's debug/dev menu site (grep the existing Debug-build-only menu items in `EditorPanels.cpp`) + `ArcaneClient` compute dispatch helper
- Modify: `docs/specs/2026-08-11-gpu-crash-diagnostics-design.md` (Testing section: mark which battery items have their trigger)

**Interfaces:**
- Produces: a `#if !defined(ARCANE_DIST)` menu item "Crash GPU (diagnostics test)" that dispatches an out-of-bounds compute write (a tiny always-faulting `.hlsl` added to `data/shaders/` following `compile-shaders.bat`'s artifact conventions), producing a real device removal for desk battery items 1-3.

- [ ] **Step 1: Implement; Debug + Release 0/0; full suite `~[gpu]` green from the exe dir; grep gate: `getNativeObject` appears ONLY in the Task 5/6/7 seam files (no wrapping crept in).**
- [ ] **Step 2: Commit** `feat(editor): deliberate GPU-fault dev command -- desk-battery trigger`
- [ ] **Step 3: Report the desk battery** (spec Testing, 5 items) as the USER's checklist verbatim in the completion summary -- the arc is NOT done until it runs. Do not claim GPU capture verified.

---

## Verification (after all tasks)

- Debug + Release 0/0; full suite `~[gpu]` green from the exe dir; both hosts link.
- Grep gates: `WriteBufferImmediate|vkCmdWriteBufferMarkerAMD|vkGetDeviceFaultInfoEXT` only in `GpuCrashD3D12.cpp`/`GpuCrashVulkan.cpp`; `SetGpuSectionProvider` called from exactly one place per host lifetime; every `// F-n:` citation resolves to the facts file.
- The desk battery (spec Testing 1-5) is the user's; surface it in the final summary.
