# Agent Verification Plan A -- Offscreen Hosts and Observation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Both hosts can boot a project, render the real frame graph deterministically with no window and no swapchain, and emit machine-readable observations an agent can assert on.

**Architecture:** Offscreen is not "no window" -- it is a window that is never shown and no swapchain. `GpuContext::Create` is unchanged (it already creates the window hidden); the two `Window::Show()` calls are guarded; the render vehicle is built by `NriGraphContext::CreateOffscreen` over a device created by `NativeDeviceOwner::Create` rather than by the windowed `Create(cfg, window)` path; and `--screenshot` reads `ReadCapture()` instead of the backbuffer.

**Tech Stack:** C++23, MSVC (VS 18), premake5, Catch2, nlohmann/json (vendored), NRI, SDL3, Dear ImGui.

## Global Constraints

- Spec: `docs/specs/2026-08-23-agent-verification-offscreen-design.md`. Read it before Task 1.
- **Everything in this plan ships in the engine, always, in every build.** Plan A is the *mode*; the **Servitor** package is Plans B and C. So: no feature macro, no premake option, and nothing here goes under `#if !defined(ARCANE_DIST)` -- offscreen verification must work in Release. See the spec's **Tiering** section; the short form is that Playwright is a package and headless Chrome is a mode of Chrome, and this plan is the mode.
- **The report JSON is the tier boundary, so it is a contract**: it carries a `schemaVersion`, like `.arcproj`'s `formatVersion`. Servitor consumes this file and does not link the engine.
- **Engine flags stay generic** -- `--offscreen`, never `--servitor-*`. Chrome does not namespace `--headless` for Playwright.
- **The flag is `--offscreen`, never `--headless`** -- "headless" already means *device-less* in this codebase (`SceneRenderResolver.hpp:211`).
- **`CreateOffscreen` is mandatory.** Never build a swapchain over a hidden window (`RuntimeApp.cpp:353-365`).
- **No flag is ever silently inert.** Honoured or refused per host, stderr + exit 2, matching `HostConfig.cpp:64-68`.
- **The engine emits facts; the agent asserts.** No assertion DSL in C++.
- **Exit code means "did the host run,"** not "did a check pass."
- `msbuild` is NOT on PATH: `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`.
- Build `Arcane.slnx`, never a bare `.vcxproj`. `ARCANE_SDK` must be set; it can be stale in the process -- override per invocation.
- Dev-loop gate is `ArcaneTests.exe "~[gpu]"`, run **from the exe dir**. GPU work at the desk.
- `[mesh]` must be run as `"[mesh]~[gpu]"` -- four cases carry real GPU tags.
- Gate to beat: **51554/1126 Debug/Release, 51436/1117 Dist, 0 warnings.**

---

## Spec -> plan coverage diff

F2a's plan was a lossy compression of its spec and nothing checked the compression, so twelve reviewers each correctly checking their task against the plan were structurally blind. This table is the remedy. **Every spec requirement appears exactly once**, owned by a task here or explicitly deferred to Plan B/C.

| # | Spec requirement | Section | Owner |
|---|---|---|---|
| R1 | `--offscreen` flag, both hosts | Layer 1 | Task 2 |
| R2 | Skip the reveal (`Win().Show()`), 2 sites | The seam | Tasks 4, 11 |
| R3 | `CreateOffscreen`, never swapchain-over-hidden-window | Mandatory | Tasks 3, 4 |
| R4 | `--screenshot` reads `ReadCapture()` | The seam | Task 5 |
| R5 | `--probe` parsing, malformed refused | Layer 2 | Task 1 (repeatable), Task 2 (collection + refusals), Task 7 (`ParseProbe`) |
| R6 | Probes: `brightness`, `luma`, `rgba`, `pick`, `census` | Layer 2 | Tasks 7, 8, 9 |
| R7 | `pick` via `FrameDesc::pickPixel`, not `--pick-probe` | Layer 2 | Task 9 |
| R8 | `--report` JSON: schemaVersion, backend, mode, framesRendered, exitReason, probes | Layer 2 | Task 7 |
| R8a | Tiering: engine mode ships always; nothing optional or `ARCANE_DIST`-gated; report JSON is the Servitor contract | Tiering | Tasks 2, 7 (+ binding on all) |
| R14 | Full-frame capture incl. ImGui chrome | Full-frame capture | Task 11 |
| R16 | Three rules; silent-inertness audit | Three rules | Task 12 |
| R17 | Fixed timestep `--fixed-dt`, refused outside `--offscreen` | Determinism 1 | Tasks 2, 6 |
| R18 | Pinned layout, committed read-only seed | Determinism 2 | Task 11 |
| R19 | Content animation disabled by construction | Determinism 3 | Task 10 |
| R20 | Settle-mode capture, bounded, explicit failure | Determinism 3 | Task 10 |
| R26 | Golden parity gate, offscreen vs windowed | Determinism 4 | Task 14 (manual); automated in **Plan B** |
| R27 | Event-Log detector | Phasing, phase 2 | Task 13 |
| R28 | Three reproduction runs | Phasing, phase 2 | Task 14 |
| R21-R25 | Comparator cascade, knobs, reference hierarchy, blessing | Determinism 4 | **Deferred: Plan B** |
| R9-R13, R15 | Identity addressing, script tier, actionability, trace | Layer 3, Trace | **Deferred: Plan C** |

**Two gaps this diff surfaced that the spec did not state**, both real work, both owned here:

- **G1 -- `Cli` cannot express a repeatable option.** `Cli::Result::Get` returns a single `std::string`, and `Cli.hpp:11-12` lists *"positional/array options"* as out of scope. The spec assumes `--probe` repeats. Owned by **Task 1**. Note the header calls these *"documented extension points, NOT built"* -- so building one is sanctioned by Cli's own design, not a violation of it.
- **G2 -- `CreateOffscreen` borrows a device, it does not create one.** Its signature is `CreateOffscreen(const HostConfig&, NriDevice& shared, uint32_t w, uint32_t h, const NodeSet&)`. Today every caller borrows the device the *windowed* context made. A host with no windowed context has no device. `NriGraphPixelTest.cpp:110-132` already has the recipe. Owned by **Task 3**.

---

## File structure

| File | Responsibility |
|---|---|
| `ArcaneCore/src/Arcane/Cli/Cli.hpp/.cpp` | Modify: add repeatable (`Many`) options |
| `ArcaneClient/src/Arcane/Host/HostConfig.hpp/.cpp` | Modify: `--offscreen`, `--fixed-dt`, `--probe`, `--report`; refusals |
| `ArcaneClient/src/Arcane/Host/OffscreenVehicle.hpp/.cpp` | **Create**: device-without-window -> `NriGraphContext` offscreen, shared by both hosts and by tests |
| `ArcaneClient/src/Arcane/Host/VerifyReport.hpp/.cpp` | **Create**: probe kinds, evaluation against a capture, JSON emission |
| `ArcaneRuntime/src/RuntimeApp.cpp`, `RuntimeFrame.cpp` | Modify: offscreen boot, guarded reveal, capture-sourced screenshot, fixed dt |
| `ArcaneEditor/src/App/EditorApp.cpp`, `EditorAppFrame.cpp` | Modify: offscreen boot, guarded reveal, pinned layout, full-frame capture |
| `ArcaneTests/src/CliTest.cpp` | Modify/Create: repeatable-option cases |
| `ArcaneTests/src/HostConfigTest.cpp` | Modify/Create: parse + refusal cases |
| `ArcaneTests/src/VerifyReportTest.cpp` | **Create**: probe parse, evaluation, JSON round-trip |
| `ArcaneTests/src/OffscreenVehicleTest.cpp` | **Create**: `[gpu]` -- vehicle builds, renders, captures |
| `scripts/check-faults.ps1` | **Create**: Event-Log fault detector |
| `ReferenceProject/Saved/verify-layout.ini` | **Create**: committed read-only editor layout seed |

---

## Task 1: Repeatable options in Cli

Closes **G1**. `--probe` must accumulate; `Cli` cannot express that today.

**Files:**
- Modify: `ArcaneCore/src/Arcane/Cli/Cli.hpp`, `ArcaneCore/src/Arcane/Cli/Cli.cpp`
- Test: `ArcaneTests/src/CliTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `Cli::Builder& Many()` marks an option repeatable; `std::vector<std::string> Cli::Result::GetMany(std::string_view name) const` returns every occurrence in command-line order (empty when absent).

- [ ] **Step 1: Write the failing test**

Append to `ArcaneTests/src/CliTest.cpp` (create it with the Catch2 include if absent):

```cpp
TEST_CASE("cli: a Many() option accumulates every occurrence in order", "[cli]")
{
    Arcane::Cli cli{ "t", "d" };
    cli.Option("probe", "", "repeatable probe").Many();

    const char* argv[] = { "t.exe", "--probe", "luma@1,2", "--probe", "census" };
    const Arcane::Cli::Result r = cli.Parse(5, const_cast<char**>(argv));

    REQUIRE(r.ok);
    const std::vector<std::string> probes = r.GetMany("probe");
    REQUIRE(probes.size() == 2);
    CHECK(probes[0] == "luma@1,2");
    CHECK(probes[1] == "census");
}

TEST_CASE("cli: GetMany on an absent option is empty, not defaulted", "[cli]")
{
    Arcane::Cli cli{ "t", "d" };
    cli.Option("probe", "", "repeatable probe").Many();
    const char* argv[] = { "t.exe" };
    const Arcane::Cli::Result r = cli.Parse(1, const_cast<char**>(argv));
    REQUIRE(r.ok);
    CHECK(r.GetMany("probe").empty());
}

TEST_CASE("cli: a non-Many option repeated is still last-wins", "[cli]")
{
    Arcane::Cli cli{ "t", "d" };
    cli.Option("backend", "dx12", "backend");
    const char* argv[] = { "t.exe", "--backend", "vulkan", "--backend", "dx12" };
    const Arcane::Cli::Result r = cli.Parse(5, const_cast<char**>(argv));
    REQUIRE(r.ok);
    CHECK(r.Get("backend") == "dx12");
}
```

- [ ] **Step 2: Run to verify it fails**

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m
```
Expected: **compile error** -- no member `Many` / `GetMany`.

- [ ] **Step 3: Implement**

In `Cli.hpp`, add to `Builder`:

```cpp
Builder& Many() { cli->m_opts[idx].many = true; return *this; }
```

Add to `Opt`: `bool many = false;`

Add to `Result`:

```cpp
std::unordered_map<std::string, std::vector<std::string>> multi; // Many() option -> occurrences
[[nodiscard]] std::vector<std::string> GetMany(std::string_view name) const;
```

Update the header's scope comment (`Cli.hpp:11-12`) -- array options are now built, so the "NOT built" list must no longer claim otherwise:

```
// Reusable engine-wide. std-only, presentation-free. Out of scope (documented
// extension points, NOT built): subcommands, config files, env-var fallback,
// positional options. REPEATABLE options ARE built -- see Many()/GetMany().
```

In `Cli.cpp`, add the accessor and record occurrences during parse. Where a long option's value is resolved, append to `multi` when `opt->many`:

```cpp
std::vector<std::string> Cli::Result::GetMany(std::string_view name) const
{
    const auto it = multi.find(std::string(name));
    return it == multi.end() ? std::vector<std::string>{} : it->second;
}
```

In the parse loop, after an option's value is validated, add:

```cpp
if (opt->many)
    r.multi[opt->name].push_back(value);
```

Leave the existing `values[name] = value` assignment in place -- a `Many()` option still answers `Get()` with its last occurrence, which keeps every existing call site working.

- [ ] **Step 4: Run to verify it passes**

Rebuild, then from the exe dir:
```
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[cli]" -d yes
```
Expected: **all pass**.

- [ ] **Step 5: Full gate + commit**

```
.\ArcaneTests.exe "~[gpu]"
```
Expected: pass, assertion count >= 51554 baseline plus the new cases.

```bash
git add ArcaneCore/src/Arcane/Cli/Cli.hpp ArcaneCore/src/Arcane/Cli/Cli.cpp ArcaneTests/src/CliTest.cpp
git commit -m "feat(cli): repeatable options via Many()/GetMany()"
```

---

## Task 2: HostConfig gains --offscreen, --fixed-dt, --probe, --report

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.hpp`, `HostConfig.cpp`
- Test: `ArcaneTests/src/HostConfigTest.cpp`

**Interfaces:**
- Consumes: `Cli::Many()` / `GetMany` (Task 1).
- Produces: on `HostConfig` -- `bool offscreen`, `double fixedDtSeconds`, `std::vector<std::string> probes` (raw, unparsed), `std::string reportPath`.

Refusals, all stderr + exit 2, matching the `--screenshot`-without-`--frames` idiom at `HostConfig.cpp:64-68`:

| Condition | Why |
|---|---|
| `--fixed-dt` without `--offscreen` | Rule 3: never silently inert |
| `--probe` without `--offscreen` | probes read a capture; there is none windowed |
| `--report` without `--offscreen` | same |
| `--probe` without `--frames N` | the capture lands on the last frame |
| `--fixed-dt` <= 0 | a non-positive step is a typo, not a duration |

- [ ] **Step 1: Write the failing test**

```cpp
namespace {
    Arcane::HostConfig::ParseOutcome ParseArgs(std::vector<const char*> args)
    {
        return Arcane::HostConfig::Parse((int)args.size(), const_cast<char**>(args.data()));
    }
}

TEST_CASE("hostconfig: --offscreen collects probes and a report path", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--offscreen", "--frames", "5",
                                 "--probe", "luma@640,360", "--probe", "census",
                                 "--report", "r.json" });
    REQUIRE(out.exitCode == 0);
    REQUIRE(out.config.has_value());
    CHECK(out.config->offscreen);
    CHECK(out.config->reportPath == "r.json");
    REQUIRE(out.config->probes.size() == 2);
    CHECK(out.config->probes[0] == "luma@640,360");
}

TEST_CASE("hostconfig: --fixed-dt defaults to 1/60 under --offscreen", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--offscreen", "--frames", "1" });
    REQUIRE(out.exitCode == 0);
    CHECK(out.config->fixedDtSeconds == Catch::Approx(1.0 / 60.0));
}

TEST_CASE("hostconfig: --fixed-dt without --offscreen is REFUSED, not ignored", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--fixed-dt", "0.016" });
    CHECK(out.exitCode == 2);
    CHECK_FALSE(out.config.has_value());
}

TEST_CASE("hostconfig: --probe without --frames is refused", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--offscreen", "--probe", "census" });
    CHECK(out.exitCode == 2);
}

TEST_CASE("hostconfig: a non-positive --fixed-dt is refused", "[hostconfig]")
{
    const auto out = ParseArgs({ "h.exe", "--offscreen", "--frames", "1", "--fixed-dt", "0" });
    CHECK(out.exitCode == 2);
}
```

- [ ] **Step 2: Run to verify it fails**

Build. Expected: compile error -- no member `offscreen`.

- [ ] **Step 3: Implement**

`HostConfig.hpp`, inside the struct (NOT under `#if !defined(ARCANE_DIST)` -- offscreen verification must work in Release):

```cpp
// Render the real frame graph with NO window shown and NO swapchain. Not
// "--headless": that word already means DEVICE-LESS in this codebase
// (SceneRenderResolver.hpp:211), and this mode is device-ful.
bool            offscreen = false;

// Fixed simulation delta, seconds. --offscreen only, refused elsewhere. The
// host loop is otherwise wall-clock (RuntimeApp.cpp:331), which makes
// `--frames N` advance the sim by however long those N frames happened to
// take -- different on a loaded CI box than an idle desk.
double          fixedDtSeconds = 1.0 / 60.0;

// Raw, unparsed `--probe` arguments in command-line order. Parsed by
// VerifyReport, not here: the kinds are that component's vocabulary.
std::vector<std::string> probes;

// Write the observation report to this JSON path. Empty = off.
std::string     reportPath = "";
```

Add `#include <vector>`.

`HostConfig.cpp`, registration beside the others:

```cpp
cli.Flag  ("offscreen",          "render with no window shown and no swapchain; "
                                 "pairs with --frames/--probe/--report");
cli.Option("fixed-dt", "0.0166666666666666666", "seconds per simulated frame "
                                 "(--offscreen only)").Type(CliType::Double);
cli.Option("probe", "",          "repeatable: luma@x,y | rgba@x,y | pick@x,y | census").Many();
cli.Option("report", "",         "write the observation report to this JSON path");
```

Mapping:

```cpp
cfg.offscreen      = r.Flag("offscreen");
cfg.fixedDtSeconds = r.GetAs<double>("fixed-dt");
cfg.probes         = r.GetMany("probe");
cfg.reportPath     = r.Get("report");
```

Refusals, after the existing `--screenshot` check:

```cpp
// Refused rather than ignored -- rule 3. A flag that silently does nothing is
// the worst failure mode an agent can meet: it exits 0 having done nothing,
// and an agent reads that as success.
const bool wantsOffscreenOnly = !cfg.probes.empty() || !cfg.reportPath.empty()
                              || r.Get("fixed-dt") != "0.0166666666666666666";
if (wantsOffscreenOnly && !cfg.offscreen)
{
    std::fprintf(stderr, "error: --fixed-dt/--probe/--report require --offscreen\n");
    return { std::nullopt, 2 };
}
if (!cfg.probes.empty() && cfg.maxFrames == 0)
{
    std::fprintf(stderr, "error: --probe requires --frames N (the capture lands on the last frame)\n");
    return { std::nullopt, 2 };
}
if (cfg.offscreen && cfg.fixedDtSeconds <= 0.0)
{
    std::fprintf(stderr, "error: --fixed-dt wants a positive number of seconds\n");
    return { std::nullopt, 2 };
}
```

> **Implementer note:** the default-string comparison above is fragile. If `Cli::Result` can report whether an option was *explicitly supplied*, prefer that and delete the string compare. Check `Cli.cpp`'s parse loop before choosing; if it cannot, add a `bool Supplied(std::string_view) const` in the same shape as `GetMany` and use it. Do not leave the string compare if a cleaner seam exists.

- [ ] **Step 4: Run to verify it passes**

```
.\ArcaneTests.exe "[hostconfig]" -d yes
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/HostConfig.hpp ArcaneClient/src/Arcane/Host/HostConfig.cpp ArcaneTests/src/HostConfigTest.cpp
git commit -m "feat(host): --offscreen, --fixed-dt, --probe, --report with parse-time refusals"
```

---

## Task 3: OffscreenVehicle -- a device with no window

Closes **G2**. `CreateOffscreen` borrows a device; nothing creates one without a window outside the pixel test.

**Files:**
- Create: `ArcaneClient/src/Arcane/Host/OffscreenVehicle.hpp`, `OffscreenVehicle.cpp`
- Test: `ArcaneTests/src/OffscreenVehicleTest.cpp`

**Interfaces:**
- Consumes: `HostConfig` (Task 2).
- Produces:
```cpp
namespace Arcane
{
    class ARCANE_API OffscreenVehicle
    {
    public:
        // Null on any failure, each step logged. Ordered: native device ->
        // NRI wrap -> offscreen graph context.
        static std::unique_ptr<OffscreenVehicle> Create(const HostConfig& cfg,
                                                        std::uint32_t width,
                                                        std::uint32_t height,
                                                        const NriGraphContext::NodeSet& nodes = {});
        ~OffscreenVehicle();
        [[nodiscard]] NriGraphContext& Graph() noexcept { return *m_ctx; }
        [[nodiscard]] NriDevice&       Device() noexcept { return *m_nri; }
    private:
        OffscreenVehicle() = default;
        // TEARDOWN ORDER: ctx first (it owns objects on the device), then the
        // NRI wrap, then the native owner. Declaration order is reverse
        // destruction order, same contract GpuContext documents.
        std::unique_ptr<NativeDeviceOwner>  m_native;
        std::unique_ptr<NriDevice>          m_nri;
        std::unique_ptr<NriGraphContext>    m_ctx;
    };
}
```

The recipe is `NriGraphPixelTest.cpp:110-132`, verbatim in shape.

- [ ] **Step 1: Write the failing test**

`ArcaneTests/src/OffscreenVehicleTest.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/OffscreenVehicle.hpp>

TEST_CASE("offscreen vehicle builds a device with no window (d3d12)", "[gpu][offscreen]")
{
    Arcane::HostConfig cfg;
    cfg.backend   = Arcane::GraphicsBackend::D3D12;
    cfg.offscreen = true;

    auto v = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(v != nullptr);
    CHECK(v->Graph().IsOffscreen());
    CHECK(v->Graph().SurfaceWidth()  == 256);
    CHECK(v->Graph().SurfaceHeight() == 128);
}

TEST_CASE("offscreen vehicle builds a device with no window (vulkan)", "[gpu][offscreen]")
{
    Arcane::HostConfig cfg;
    cfg.backend   = Arcane::GraphicsBackend::Vulkan;
    cfg.offscreen = true;

    auto v = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(v != nullptr);
    CHECK(v->Graph().IsOffscreen());
}
```

- [ ] **Step 2: Run to verify it fails**

Build. Expected: cannot open `Arcane/Host/OffscreenVehicle.hpp`.

- [ ] **Step 3: Implement**

`OffscreenVehicle.cpp`:

```cpp
std::unique_ptr<OffscreenVehicle> OffscreenVehicle::Create(const HostConfig& cfg,
                                                           std::uint32_t width,
                                                           std::uint32_t height,
                                                           const NriGraphContext::NodeSet& nodes)
{
    std::unique_ptr<OffscreenVehicle> v(new OffscreenVehicle());

    RenderDeviceDesc desc;
    desc.backend = cfg.backend;
    v->m_native = NativeDeviceOwner::Create(desc);
    if (!v->m_native) { ARC_ERROR("[offscreen] no usable adapter for the requested backend"); return nullptr; }

    v->m_nri = NriDevice::Wrap(*v->m_native);
    if (!v->m_nri) { ARC_ERROR("[offscreen] NriDevice::Wrap failed"); return nullptr; }

    // vsync is meaningless with nothing to present to; CreateOffscreen ignores
    // it and the other surface-owned knobs.
    v->m_ctx = NriGraphContext::CreateOffscreen(cfg, *v->m_nri, width, height, nodes);
    if (!v->m_ctx) { ARC_ERROR("[offscreen] CreateOffscreen failed at {}x{}", width, height); return nullptr; }

    ARC_INFO("[offscreen] vehicle ready: {}x{} backend={} -- no window, no swapchain",
             width, height, cfg.backend == GraphicsBackend::Vulkan ? "Vulkan" : "D3D12");
    return v;
}

OffscreenVehicle::~OffscreenVehicle() = default;
```

Add both files to the premake file list if it is not a glob -- check `premake5.lua` before assuming.

- [ ] **Step 4: Run to verify it passes**

At the desk (this is a `[gpu]` test):
```
.\ArcaneTests.exe "[offscreen]" -d yes
```
Expected: both cases pass, both backends create a device.

- [ ] **Step 5: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/OffscreenVehicle.hpp ArcaneClient/src/Arcane/Host/OffscreenVehicle.cpp ArcaneTests/src/OffscreenVehicleTest.cpp
git commit -m "feat(host): OffscreenVehicle -- device + graph context with no window"
```

---

## Task 4: ArcaneRuntime boots offscreen

**Files:**
- Modify: `ArcaneRuntime/src/RuntimeApp.cpp:353-370`

**Interfaces:**
- Consumes: `OffscreenVehicle::Create` (Task 3), `cfg.offscreen` (Task 2).
- Produces: a runtime that renders N frames with no window shown.

`:366` is `m_gpu->Win().Show();` and `:368` is `m_graphContext = NriGraphContext::Create(m_config, m_gpu->Win());` -- adjacent lines.

- [ ] **Step 1: Implement the guarded reveal and the offscreen vehicle**

```cpp
// THE REVEAL -- skipped entirely under --offscreen. The window still EXISTS
// (GpuContext created it hidden, GpuContext.hpp:46-48) so ImGuiLayer,
// InputDevices and the event pump keep working unchanged; it is simply never
// mapped, and no swapchain is ever built over it.
//
// Deliberately NOT "hidden window + ordinary swapchain": the comment below
// this one warns that "a surface created against a window the compositor has
// never mapped is exactly the kind of backend-specific corner a desk-only
// machine cannot pre-clear." CreateOffscreen builds no surface at all.
if (!m_config.offscreen)
    m_gpu->Win().Show();

if (m_config.offscreen)
{
    m_offscreen = Arcane::OffscreenVehicle::Create(
        m_config, m_gpu->Win().Width(), m_gpu->Win().Height());
    if (!m_offscreen) { /* existing failure path */ }
}
else
{
    m_graphContext = Arcane::NriGraphContext::Create(m_config, m_gpu->Win());
    if (!m_graphContext) { /* existing failure path */ }
}
```

> **Implementer note:** `Window::Width()/Height()` may not exist under those names. Verify against `Window.hpp` before writing this; if absent, use the same source the windowed path uses for its initial extent. Do not invent an accessor.

Add `std::unique_ptr<Arcane::OffscreenVehicle> m_offscreen;` to `RuntimeApp.hpp`, declared **after** `m_gpu` so it destructs first.

Every later use of `m_graphContext` in the frame path must resolve to whichever context is live. Introduce one accessor rather than branching at each site:

```cpp
[[nodiscard]] Arcane::NriGraphContext& Graph() noexcept
{
    return m_offscreen ? m_offscreen->Graph() : *m_graphContext;
}
```

**The frame path reaches it through the frame IO struct, not through `RuntimeApp`.** `RuntimeFrame.cpp` takes an `io` (it already carries `io.gpu` and `io.config` -- see `RuntimeFrame.cpp:91`). Add the resolved context to that struct at the call site so the frame code never branches on mode:

```cpp
// In the frame IO struct declaration:
Arcane::NriGraphContext& graph;   // the LIVE context -- offscreen or windowed

// At the call site in RuntimeApp, where the io is built:
io.graph = Graph();
```

Later tasks in this plan write `io.graph.ReadCapture(...)`. Use exactly that name.

> **Implementer note:** find the actual frame IO type before editing -- it is the struct `RuntimeFrame.cpp:91` reads `io.gpu` from. If it holds a pointer rather than a reference elsewhere, match the surrounding convention rather than introducing a reference member into a struct that is assigned.

- [ ] **Step 2: Build**

Expected: 0 errors, 0 warnings.

- [ ] **Step 3: Run it**

```
bin\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject --offscreen --frames 5 --backend dx12
```
Expected: exit 0. Log shows `[offscreen] vehicle ready` and **no** `Window created` reveal effect -- no window appears on screen. The census line still prints.

- [ ] **Step 4: Prove the windowed path is unchanged**

```
bin\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject --frames 5 --backend dx12
```
Expected: exit 0, window appears as before. **This is the regression check that matters** -- offscreen must be additive.

- [ ] **Step 5: Commit**

```bash
git add ArcaneRuntime/src/RuntimeApp.cpp ArcaneRuntime/src/RuntimeApp.hpp
git commit -m "feat(runtime): boot offscreen -- guarded reveal, OffscreenVehicle, no swapchain"
```

---

## Task 5: --screenshot reads the capture, not the backbuffer

**Files:**
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp:439`, `:498-521`

`:513` currently logs `"screenshot FAILED: {} (backbuffer readback)"`. `:439` already sets `graphFrame.capture` on the last frame, so the arming logic exists; only the source changes.

- [ ] **Step 1: Implement**

```cpp
if (lastFrame && !io.config.screenshotPath.empty())
{
    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> actual;
    // ReadCapture in BOTH modes: offscreen has no backbuffer, and the windowed
    // path's capture is the same normalized RGBA. Its contract is explicit
    // that a false is NOT a run failure -- "a capture that could not be read
    // is not a bad frame" -- so this warns and continues rather than failing.
    if (!io.graph.ReadCapture(w, h, actual))
    {
        ARC_WARN("screenshot FAILED: {} (no capture landed)", io.config.screenshotPath);
    }
    else if (Arcane::WritePngRgba(io.config.screenshotPath, w, h, actual.data()))
    {
        ARC_INFO("screenshot written: {} ({}x{})", io.config.screenshotPath, w, h);
    }
    else
    {
        ARC_WARN("screenshot FAILED: {}", io.config.screenshotPath);
    }
}
```

- [ ] **Step 2: Run offscreen**

```
ArcaneRuntime.exe --project ReferenceProject --offscreen --frames 5 --backend dx12 --screenshot off.png
```
Expected: exit 0, `off.png` written, log reports its dimensions.

- [ ] **Step 3: Run windowed**

```
ArcaneRuntime.exe --project ReferenceProject --frames 5 --backend dx12 --screenshot win.png
```
Expected: exit 0, `win.png` written.

- [ ] **Step 4: Eyeball both**

Open both PNGs. They must show the same scene. **This is the first evidence for Task 14's parity check** -- if they differ grossly, stop and investigate before continuing.

- [ ] **Step 5: Commit**

```bash
git add ArcaneRuntime/src/RuntimeFrame.cpp
git commit -m "feat(runtime): --screenshot reads ReadCapture in both modes"
```

---

## Task 6: Fixed timestep under --offscreen

**Files:**
- Modify: `ArcaneRuntime/src/RuntimeApp.cpp:331`, and the sim-advance site that consumes `simPrev`

- [ ] **Step 1: Implement**

```cpp
// Under --offscreen the sim advances by a FIXED delta, not wall clock.
// Otherwise `--frames 5` advances by however long those five frames took,
// which differs between a loaded CI box and an idle desk -- and makes pixel
// assertions on anything animated flaky by construction.
const double dt = m_config.offscreen
                ? m_config.fixedDtSeconds
                : std::chrono::duration<double>(now - simPrev).count();
```

> **Implementer note:** find the actual consumer of `simPrev` before editing -- the variable is declared at `:331` but used further down, and `RuntimeFrame.cpp:154,176` take their own `steady_clock::now()`. **All** sim-advancing deltas must switch, or the mode is only half deterministic. Grep `steady_clock` across `ArcaneRuntime/src` and account for every hit; a render-timing-only clock may legitimately stay wall-clock, but say so in a comment where you leave one.

- [ ] **Step 2: Prove determinism**

```
ArcaneRuntime.exe --project ReferenceProject --offscreen --frames 30 --backend dx12 --screenshot a.png
ArcaneRuntime.exe --project ReferenceProject --offscreen --frames 30 --backend dx12 --screenshot b.png
fc /b a.png b.png
```
Expected: **"no differences encountered"** -- byte-identical. Same format, same path, same fixed dt.

If they differ, do not proceed: something is still wall-clock or otherwise nondeterministic. Bisect by grepping the remaining `steady_clock` and `rand` uses in the frame path.

- [ ] **Step 3: Commit**

```bash
git add ArcaneRuntime/src/RuntimeApp.cpp ArcaneRuntime/src/RuntimeFrame.cpp
git commit -m "feat(runtime): fixed timestep under --offscreen"
```

---

## Task 7: VerifyReport -- probe parsing, luma/rgba, JSON emission

**Files:**
- Create: `ArcaneClient/src/Arcane/Host/VerifyReport.hpp`, `VerifyReport.cpp`
- Test: `ArcaneTests/src/VerifyReportTest.cpp`

**Interfaces:**
- Consumes: `cfg.probes`, `cfg.reportPath` (Task 2); `ReadCapture` output.
- Produces:
```cpp
namespace Arcane
{
    enum class ProbeKind : std::uint8_t { Luma, Rgba, Pick, Census };

    struct ProbeSpec
    {
        ProbeKind    kind{};
        std::int32_t x = 0, y = 0;   // only meaningful for Luma/Rgba/Pick
        std::string  raw;            // as typed, echoed into the report
    };

    // nullopt + a message on a malformed spec. Refusal, never a silent skip.
    [[nodiscard]] std::optional<ProbeSpec> ParseProbe(std::string_view text, std::string& error);

    class VerifyReport
    {
    public:
        void SetRun(std::string backend, bool offscreen, std::uint64_t framesRendered,
                    std::string exitReason);
        void SetCapture(std::uint32_t w, std::uint32_t h, const std::vector<unsigned char>& rgba);
        void AddCensus(int spriteReferenced, int spriteBound, bool postReferenced,
                       bool postBound, int meshReferenced, int meshBound);
        // Evaluates every spec against the capture set by SetCapture.
        void Evaluate(const std::vector<ProbeSpec>& specs);
        [[nodiscard]] std::string ToJson() const;
        [[nodiscard]] bool WriteTo(const std::string& path) const;
    };
}
```

Luma is the same linear-intensity reading the `[gpu][pixel]` cases use -- reuse `NriGraphPixelTest.cpp`'s `Luma` formula rather than inventing a second one. Read it and copy the expression exactly.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("verify: probe specs parse, and malformed ones are refused", "[verify]")
{
    std::string err;
    const auto luma = Arcane::ParseProbe("luma@640,360", err);
    REQUIRE(luma.has_value());
    CHECK(luma->kind == Arcane::ProbeKind::Luma);
    CHECK(luma->x == 640);
    CHECK(luma->y == 360);

    const auto census = Arcane::ParseProbe("census", err);
    REQUIRE(census.has_value());
    CHECK(census->kind == Arcane::ProbeKind::Census);

    CHECK_FALSE(Arcane::ParseProbe("census@1,2", err).has_value());   // args to an argless kind
    CHECK_FALSE(Arcane::ParseProbe("luma", err).has_value());         // kind missing its args
    CHECK_FALSE(Arcane::ParseProbe("luma@-1,2", err).has_value());    // negative is a typo
    CHECK_FALSE(Arcane::ParseProbe("bogus@1,2", err).has_value());    // unknown kind
    CHECK_FALSE(err.empty());
}

TEST_CASE("verify: a luma probe reads the capture and lands in the JSON", "[verify]")
{
    // 2x1 capture: black pixel, white pixel.
    const std::vector<unsigned char> rgba = { 0,0,0,255,  255,255,255,255 };

    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", /*offscreen=*/true, /*framesRendered=*/5, "frames-complete");
    rep.SetCapture(2, 1, rgba);

    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("luma@0,0", err), *Arcane::ParseProbe("luma@1,0", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    // The report is the boundary between the engine tier and the Servitor
    // package, which parses this file without linking the engine -- so the
    // version is part of the contract, not decoration.
    CHECK(doc["schemaVersion"] == 1);
    CHECK(doc["backend"] == "D3D12");
    CHECK(doc["mode"] == "offscreen");
    CHECK(doc["framesRendered"] == 5);
    CHECK(doc["exitReason"] == "frames-complete");
    REQUIRE(doc["probes"].size() == 2);
    CHECK(doc["probes"][0]["value"].get<double>() == Catch::Approx(0.0).margin(0.01));
    CHECK(doc["probes"][1]["value"].get<double>() == Catch::Approx(1.0).margin(0.01));
}

TEST_CASE("verify: an out-of-bounds probe reports a fact, it does not crash", "[verify]")
{
    Arcane::VerifyReport rep;
    rep.SetRun("D3D12", true, 1, "frames-complete");
    rep.SetCapture(2, 1, { 0,0,0,255, 255,255,255,255 });
    std::string err;
    rep.Evaluate({ *Arcane::ParseProbe("luma@99,99", err) });

    const auto doc = nlohmann::json::parse(rep.ToJson());
    REQUIRE(doc["probes"].size() == 1);
    CHECK(doc["probes"][0].contains("error"));
    CHECK_FALSE(doc["probes"][0].contains("value"));
}
```

- [ ] **Step 2: Run to verify it fails**

Build. Expected: cannot open `Arcane/Host/VerifyReport.hpp`.

- [ ] **Step 3: Implement**

Follow `ProjectBoot.hpp:116-132`'s `EngineInfoJson` precedent exactly -- `nlohmann::json j; j["k"] = v; return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);`. The `error_handler_t::replace` matters for the same reason it does there: a malformed byte must degrade, never throw out of `main`.

Report shape:

```json
{ "schemaVersion": 1,
  "backend": "D3D12", "mode": "offscreen", "framesRendered": 5,
  "exitReason": "frames-complete",
  "capture": { "width": 1280, "height": 720 },
  "census": { "spriteReferenced": 4, "spriteBound": 1, "postReferenced": false,
              "postBound": false, "meshReferenced": 1, "meshBound": 1 },
  "probes": [ { "raw": "luma@640,360", "kind": "luma", "x": 640, "y": 360, "value": 0.71 } ] }
```

Every probe entry carries either `value`/`entity` **or** `error` -- never neither, never both.

- [ ] **Step 4: Run to verify it passes**

```
.\ArcaneTests.exe "[verify]" -d yes
```

- [ ] **Step 5: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/VerifyReport.hpp ArcaneClient/src/Arcane/Host/VerifyReport.cpp ArcaneTests/src/VerifyReportTest.cpp
git commit -m "feat(host): VerifyReport -- probe parsing, luma/rgba evaluation, JSON emission"
```

---

## Task 8: Wire the report into the runtime, plus the census probe

**Files:**
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp` (capture site), `RuntimeApp.cpp` (shutdown/report write)
- Modify: `ArcaneClient/src/Arcane/Host/VerifyReport.cpp` (census serialisation)

`SceneRenderResolver::Materials()` returns a 6-field POD (`SceneRenderResolver.hpp:184-217`), only stringified today at `SceneRenderResolver.cpp:423`.

- [ ] **Step 1: Implement**

At the same last-frame point that reads the capture, populate and write the report. Carry the census from `Materials()`.

**`spriteBound` is compiler/batcher-gated** (`SceneRenderResolver.hpp:211-214`) and can read 0 for reasons unrelated to correctness. The report's `mode` field is what lets an agent interpret that, so it must always be present.

- [ ] **Step 2: Run it**

```
ArcaneRuntime.exe --project ReferenceProject --offscreen --frames 5 --backend dx12 ^
  --probe census --probe luma@640,360 --report r.json
```
Expected: exit 0, `r.json` written.

- [ ] **Step 3: Verify the report against pinned ground truth** *(rewritten 2026-08-23 — see below)*

> **The original criterion was wrong and has been replaced.** It read: *"The
> census in `r.json` must match the `scene resolution:` log line exactly."*
> That is wrong three ways. **(a) Unsatisfiable as written** — `4
> SpriteRenderer(s)` and `1 bound mesh material(s)` have no census field at all.
> **(b) A category error** — the log mixes 3 component counters with 4
> `Table().size()` values, which are **per-distinct-asset**, while `Materials()`
> is **per-component** throughout; two MeshRenderers sharing one `.arcmesh` give
> 2 vs 1. Exactly **one** pair shares a definition: `N material guid(s)` ≡
> `spriteReferenced`. **(c) Racy even where definitions align** — the log line
> fires only from the change-detector (`SceneRenderResolver.cpp:412-416`), so
> "the last line" is the last *change*, at an arbitrary early frame, being
> compared against a shutdown-time census. This is what made `spriteBound: 1`
> look like it contradicted `0 bound material(s)`: the `0` was frame 1, and the
> same run logged `1` seventy-six milliseconds later.

The census in `r.json` must equal, field for field, the values
`ArcaneTests/src/HostBootTest.cpp` already pins for ReferenceProject:
`spriteReferenced=1`, `meshReferenced=1`, `postReferenced=true` (the cold case,
pure scene data) and `meshBound=1` (the mesh-resolution case).

For a **settled** run (`--frames >= 45`, clear of the shader-compile race)
additionally assert the *invariant* the POD exists to express, rather than
literals:

```
spriteBound == spriteReferenced
meshBound   == meshReferenced
```

Use the log only as **bounds**, never equality:
`spriteBound <= spriteReferenced`, `meshBound <= meshReferenced`, and
`spriteReferenced == <the log's "N material guid(s)">` — the one pair that
genuinely shares a definition.

> **Do NOT yet assert `postBound == postReferenced`.** A settled 60-frame run on
> **both** backends reports `postReferenced: true, postBound: false` for
> ReferenceProject — a declared post chain that never binds. That is an open
> question (see Task 8's outcome notes), and the invariant would fail today.

- [ ] **Step 4: Commit**

```bash
git add ArcaneRuntime/src/RuntimeFrame.cpp ArcaneRuntime/src/RuntimeApp.cpp ArcaneClient/src/Arcane/Host/VerifyReport.cpp
git commit -m "feat(runtime): emit the observation report, including the scene census"
```

---

## Task 9: The pick probe, driven by FrameDesc::pickPixel

**Files:**
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp`, `VerifyReport.cpp`

**Not** the runtime's `--pick-probe` flag. `ArcaneEditor/src/main.cpp:92-98` records that `NriGraphContext` *"refuses to latch the flag on an offscreen (viewport) context."* The editor's own pick works offscreen on every click through `FrameDesc::pickPixel` (`EditorAppFrame.cpp:1643`). Drive that.

- [ ] **Step 1: Implement**

Arm `FrameDesc::pickPixel` from the first `pick@x,y` spec. The readback lands with frames-in-flight latency -- `RuntimeApp.cpp:544` already has the "NO READBACK LANDED" precedent and its message; reuse that wording so the two paths read alike.

The report entry carries **both** `entity` (the resolved name) and `id`, per the spec's discoverability rule: a raw GUID cannot be guessed, so an agent bootstraps from a readable query and then addresses durably.

- [ ] **Step 2: Run it**

```
ArcaneRuntime.exe --project ReferenceProject --offscreen --frames 5 --backend dx12 ^
  --probe pick@640,360 --report pick.json
```
Expected: exit 0; `pick.json` names the entity at screen centre, or reports a background miss as a *fact* with `id: 0`.

- [ ] **Step 3: Cross-check against the screenshot**

Add `--screenshot pick.png` and confirm the entity named at (640,360) is what is visibly at that pixel.

- [ ] **Step 4: Commit**

```bash
git add ArcaneRuntime/src/RuntimeFrame.cpp ArcaneClient/src/Arcane/Host/VerifyReport.cpp
git commit -m "feat(runtime): pick probe via FrameDesc::pickPixel, reporting name and id"
```

---

## Task 10: Settle-mode capture and content animation

**Files:**
- Modify: `HostConfig.hpp/.cpp` (add `--settle N`), `RuntimeFrame.cpp`

Fixing the clock makes *time* deterministic, not *content*. Async resource loads, shader compilation and cache warm-up all complete on wall-clock schedules the sim clock does not control.

- [ ] **Step 1: Implement `--settle N`**

Repeat the capture until two consecutive rendered frames compare **equal**, up to N attempts. On non-convergence, **fail explicitly** and record `exitReason: "settle-not-converged"` -- never silently return the last frame. `--settle` outside `--offscreen` is refused, same as `--fixed-dt`.

Comparison here is byte equality: same mode, same format, same path, so bitwise is correct and no comparator is needed. (Cross-format comparison is Plan B.)

- [ ] **Step 2: Test non-convergence is explicit**

Point `--settle 3` at a scene with a continuously animating element and confirm the run reports non-convergence rather than passing.

- [ ] **Step 3: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/HostConfig.hpp ArcaneClient/src/Arcane/Host/HostConfig.cpp ArcaneRuntime/src/RuntimeFrame.cpp
git commit -m "feat(runtime): settle-mode capture with explicit non-convergence"
```

---

## Task 11: ArcaneEditor boots offscreen, with pinned layout and full-frame capture

The valuable host and the harder one -- editor-side `Draw()` is where F2a's bugs actually were.

**Files:**
- Modify: `ArcaneEditor/src/App/EditorApp.cpp:1392` (reveal), `:959`, `:971` (layout), `EditorAppFrame.cpp:1334-1410` (capture)
- Create: `ReferenceProject/Saved/verify-layout.ini`

- [ ] **Step 1: Guard the reveal and build the offscreen vehicle**

Same shape as Task 4, at `EditorApp.cpp:1392`.

- [ ] **Step 2: Pin the layout**

`EditorApp.cpp:971` sets `io.IniFilename = m_layoutIniPath.c_str()` -- "the PER-PROJECT LAYOUT FILE" (`:487`) -- and `:959` writes it back on exit. Under `--offscreen`:

```cpp
// PINNED. The per-project layout is USER STATE: it differs per machine and
// :959 writes it back on exit, so an agent run would both read a layout it
// did not choose and MUTATE it, making run N+1 differ from run N.
// :492 already names this "the imgui.ini veto class, which is the worst kind
// of bug." Pinning precedent is :455.
if (m_config.offscreen)
{
    io.IniFilename = nullptr;                       // never read, never written
    ImGui::LoadIniSettingsFromDisk(verifyLayoutPath.c_str());   // one committed seed
}
```

Guard `:959`'s `SaveIniSettingsToDisk` so it cannot fire under `--offscreen`.

- [ ] **Step 3: Capture the full composited frame**

`ArcaneEditor/src/main.cpp:87-88` records that `--screenshot` *"captures the VIEWPORT panel's texture, not the editor window"* -- so the Inspector and asset browser are invisible today. Under `--offscreen`, capture the composited frame **including ImGui chrome**.

This is tractable because `ImGuiNri::Init` takes a device and pipeline cache, **not a window** (`ImGuiNri.hpp:159`), and the editor sets only `ImGuiConfigFlags_DockingEnable` (`EditorApp.cpp:404`) with **no `ViewportsEnable`** -- so ImGui never spawns OS windows for floating panels.

- [ ] **Step 4: Generate the layout seed**

Run the editor windowed once, arrange the panels a verification run should see, and copy the resulting ini to `ReferenceProject/Saved/verify-layout.ini`. **Commit it** -- it is shared by every agent run on every machine.

- [ ] **Step 5: Run it**

```
ArcaneEditor.exe --project ReferenceProject --offscreen --frames 10 --backend dx12 --screenshot editor.png
```
Expected: exit 0, no window appears, and `editor.png` shows **the whole editor** -- panels, docking, asset browser -- not just the viewport.

- [ ] **Step 6: Prove the layout is not mutated**

```
git status ReferenceProject/
```
Expected: **clean**. If the layout file changed, `:959`'s guard is wrong.

- [ ] **Step 7: Commit**

```bash
git add ArcaneEditor/src/App/EditorApp.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ReferenceProject/Saved/verify-layout.ini
git commit -m "feat(editor): offscreen boot, pinned layout, full-frame capture with ImGui chrome"
```

---

## Task 12: Silent-inertness audit

Rule 3. For an agent, a flag that exits 0 having done nothing reads as success -- the worst failure mode there is.

**Files:**
- Modify: `ArcaneEditor/src/main.cpp`, `ArcaneClient/src/Arcane/Host/HostConfig.cpp`

- [ ] **Step 1: Enumerate**

`ArcaneEditor/src/main.cpp:85-100` already documents the editor's dispositions. Known inert: `--pick-probe`, `--perf`. Re-derive the list rather than trusting this one -- it may have drifted.

- [ ] **Step 2: Refuse rather than ignore, in each host's main**

**The refusal cannot live in shared `HostConfig`**: `Parse(argc, argv)` takes no host identity, so it cannot know which host is asking. Put it in each host's `main.cpp` immediately after parse -- the same place `printEngineInfo` is already handled (`ArcaneRuntime/src/main.cpp:36`, `ArcaneEditor/src/main.cpp:122`).

In `ArcaneEditor/src/main.cpp`, after the parse succeeds:

```cpp
// Rule 3: honoured or refused, never silently inert. Both of these parse
// fine and then do NOTHING on this host, exiting 0 -- which an agent reads
// as success. main.cpp:92-100 documented the inertness; this enforces it.
if (parsed.config->pickProbe)
{
    std::fprintf(stderr, "error: --pick-probe is a RUNTIME flag; the editor's pick is a live "
                         "click through FrameDesc::pickPixel. Use ArcaneRuntime.exe.\n");
    return 2;
}
if (parsed.config->perf)
{
    std::fprintf(stderr, "error: --perf is not implemented on the editor host "
                         "(FramePerf is constructed and never sampled). Use ArcaneRuntime.exe.\n");
    return 2;
}
```

Then update the disposition comments at `ArcaneEditor/src/main.cpp:92-100` -- they currently say "PARSED AND INERT ON THIS HOST", which becomes false. Replace with "REFUSED ON THIS HOST" and the reason.

`--nri-graph` is the **documented exception**: it is deliberately parsed-and-ignored so saved launch args and Hub-stored command lines do not fail to boot (`HostConfig.cpp:18-28`). That reasoning still stands -- leave it, and add a line to its comment saying it is a deliberate carve-out from rule 3 rather than an oversight.

- [ ] **Step 3: Verify by exit code**

These are `main.cpp` behaviours, not unit-testable in `ArcaneTests`. Assert them by running the exe:

```
bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project ReferenceProject --pick-probe 10,10 --frames 1
echo exit=%ERRORLEVEL%
```
Expected: **exit 2** with the message above, and **no window**.

```
bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project ReferenceProject --nri-graph --frames 1
echo exit=%ERRORLEVEL%
```
Expected: **exit 0** -- the carve-out still boots.

- [ ] **Step 4: Commit**

```bash
git add ArcaneEditor/src/main.cpp ArcaneClient/src/Arcane/Host/HostConfig.cpp ArcaneTests/src/HostConfigTest.cpp
git commit -m "fix(host): refuse per-host inert flags instead of silently ignoring them"
```

---

## Task 13: The Event-Log fault detector

Spec phase 2, deliverable 1. The signature `nvwgf2umx.dll +0x1203ce` sat in the Windows Event Log for six weeks with nobody reading it. This makes a recurrence announce itself.

**Files:**
- Create: `scripts/check-faults.ps1`

- [ ] **Step 1: Implement**

```powershell
# Reports Application Error faults for Arcane executables, newest first.
# The Parsec/GPU driver fault (nvwgf2umx.dll +0x1203ce) never reaches the
# engine's own crash capture -- it is inside the driver -- so the Windows
# Event Log is the only instrument that sees it.
param([int]$Days = 30)
$ErrorActionPreference = 'SilentlyContinue'
$ev = Get-WinEvent -FilterHashtable @{
    LogName='Application'; ProviderName='Application Error'
    StartTime=(Get-Date).AddDays(-$Days)
} -MaxEvents 500
if (-not $ev) { "No Application Error events in the last $Days days."; exit 0 }
$rows = $ev | ForEach-Object {
    $p = $_.Properties
    [pscustomobject]@{ Time=$_.TimeCreated; App=$p[0].Value; Faulting=$p[3].Value; Offset=$p[7].Value }
} | Where-Object { $_.App -like 'Arcane*' }
if (-not $rows) { "No Arcane faults in the last $Days days."; exit 0 }
$rows | Sort-Object Time -Descending | Format-Table -AutoSize
$driver = $rows | Where-Object { $_.Faulting -like 'nv*' -or $_.Faulting -like 'amd*' }
if ($driver) { Write-Warning "GPU DRIVER faults present -- $($driver.Count). This is the Parsec-class signature." }
```

- [ ] **Step 2: Run it**

```
powershell -File scripts\check-faults.ps1 -Days 60
```
Expected: lists the known recent faults. Over 60 days on the dev box it should show the `2026-08-22` `ArcaneTests.exe` self-faults and **no** `nvwgf2umx` entries.

- [ ] **Step 3: Commit**

```bash
git add scripts/check-faults.ps1
git commit -m "chore(scripts): Event-Log fault detector for the driver-crash signature"
```

---

## Task 14: Desk checkpoint -- parity and the reproduction attempt

**USER-DRIVEN.** Do not mark complete without the user at the desk.

- [ ] **Step 1: Offscreen vs windowed, both backends**

For each of `dx12` and `vulkan`, capture windowed and offscreen at the same extent and compare by eye. They must show the same scene. Automated comparison is Plan B; this is the one-time foundational check.

- [ ] **Step 2: Editor offscreen full-frame**

Confirm `editor.png` shows the whole editor UI, and that `git status ReferenceProject/` stays clean.

- [ ] **Step 3: Determinism**

```
fc /b a.png b.png
```
Byte-identical across two identical offscreen runs.

- [ ] **Step 4: The reproduction attempt** (spec phase 2, deliverable 2)

With **Parsec active**, run the windowed `[gpu]` suite three consecutive times at the desk:
```
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[gpu]" -d yes
```
Then `powershell -File scripts\check-faults.ps1 -Days 1`.

Outcome is *"reproduced, trigger characterised"* or *"not reproduced in three runs, recorded dormant, detector in place."* **A negative result is a valid and expected outcome, not a failure of the task.**

- [ ] **Step 5: Full gate**

```
.\ArcaneTests.exe "~[gpu]"
```
Expected: >= 51554 assertions, 0 failures, 0 warnings in the build.

- [ ] **Step 6: Record the outcome**

Append a short outcome section to this plan -- what passed, what the reproduction attempt showed, and any deviation. That record is what Plan B and Plan C inherit.

---

## Deferred, by design

- **Plan B** -- the comparator cascade (CIE94, 3x3 variance, SSIM), its knobs, the backend-keyed reference-image hierarchy, and the blessing path. Until it lands, parity is the manual check in Task 14.
- **Plan C** -- identity addressing (`###` widget ids, entity GUIDs, strict resolution), the script tier, actionability checks, and the trace bundle.


---

# Task 14 - CONSOLIDATED DESK CHECKLIST

**Assembled 2026-08-24 from every item banked during execution.** Windowed runs
were forbidden throughout (a known NVIDIA driver fault under this box's live
Parsec session), so **every windowed path in this branch is unverified by
design.** This list is what closes that. Each line is a behaviour that was
changed or newly created and never executed.

Run `powershell -File scripts\check-faults.ps1 -Days 1` after each windowed block.

## A. The foundational parity check (the arc's one-time claim)

- [ ] For **each** of `dx12` and `vulkan`, capture windowed and offscreen at the
      same extent and compare by eye. Offscreen images from this build are under
      `.superpowers/sdd/2026-08-23-agent-verification-offscreen-hosts/evidence/`.
- [ ] **Do not rest parity on flat regions.** A previous "cross-backend parity"
      result here was an artifact of sampling a pixel >=20 px inside a
      uniformly-shaded quad, and the editor capture is 42% flat chrome + 28%
      flat viewport clear, no gradients, no AA. Compare **edge and gradient**
      pixels, or a whole-image/tile hash.

## B. Windowed regression - the runtime (Tasks 4, 5, 6)

- [ ] `ArcaneRuntime.exe --project ReferenceProject --frames 5 --backend dx12`
      - window appears, exit 0. **This is the check that matters**: offscreen was
      supposed to be purely additive.
- [ ] `--frames 5 --backend dx12 --screenshot win.png`, then eyeball against the
      offscreen PNG.
- [ ] Seven behaviours changed inside windowed branches, never executed: reveal
      ordering/raise; `FrameExtent`'s null-`m_swap` yielding a silent 0x0 rather
      than a loud failure; the resolved-graph ternary's false branch; live resize
      reporting; windowed teardown latch; windowed `ProbeId`; **Release/Dist**
      (only Debug was exercised at runtime).

## C. Windowed regression - the editor (Tasks 11, 12)

- [ ] Editor smoke run: boots windowed, opens a scene **plus a Mesh or Shader
      document preview**, no `SetGpuHeartbeatPublisher` WARN, `RenderErrorCount
      0 -> 0`, and a viewport drag-storm produces **no spurious `gpu-stall`
      .arcdiag**. The Task 4 fix widened the shared surface into
      `NriGraphContext.cpp`/`HostConfig.cpp`; its new WARN branch has never run.
- [ ] `ArcaneEditor.exe --project ReferenceProject --nri-graph --frames 1`
      - expect **exit 0**; the deliberate rule-3 carve-out must still boot.
      Reasoned from source only.
- [ ] The rival-editor-lock `Diagnostics::Shutdown()` fix was applied **by
      analogy** to the measured no-project case, never reproduced. Needs a second
      live windowed editor to confirm it exits 2 rather than hanging on the modal.

## D. The layout seed - WITH its falsifiability condition (Task 11)

`ReferenceProject/Saved/verify-layout.ini` is committed but **inert**: its two
entries name `Debug##Default` and `DockSpaceViewport`, while this editor's dock
host window is `EditorDockHost`. **Zero of two entries match a live window**, so
`LoadIniSettingsFromDisk` has never applied a single setting. The capture looks
correct only because `EditorPanels.cpp:389` runs `BuildDefaultLayout` when
`DockBuilderGetNode` is null.

- [ ] Author the layout **visibly different from `BuildDefaultLayout`** (e.g.
      Inspector docked left). **This is the falsifiability condition** - with an
      identical layout the test cannot distinguish "ini restored" from
      "programmatic default ran", and would pass either way.
- [ ] Commit it, then verify: (a) the PINNED log line fires; (b)
      `DockBuilderGetNode` is **non-null on frame 1** so `BuildDefaultLayout`
      does NOT run; (c) the capture shows the authored arrangement; (d) two runs
      byte-identical **and** the seed's SHA unchanged.
- [ ] `git status ReferenceProject/` clean afterwards. **Snapshot it BEFORE the
      run and diff against that** - `EditorApp::MintMeshAsset` writes an
      untracked `New Mesh.arcmesh` into the real project and is not gitignored.

## E. Offscreen paths never exercised (Task 11)

- [ ] `--offscreen` + a **project switch**. Note ImGui applies ini settings at
      window *creation*, so a mid-session `LoadIniSettingsFromDisk` is partially
      inert anyway - already-created panels will not move.
- [ ] `--offscreen` **with a document open** - the case that makes the
      four-context heartbeat topology concrete.

## F. The pick probe, windowed (Task 9)

- [ ] `ArcaneRuntime --report r.json --probe pick@640,360` **without**
      `--offscreen`: confirm the "this run is WINDOWED" WARN fires, exit 0, and
      `r.json` carries `"mode":"windowed"` with the no-pick-set error and no
      `entity`/`pickableKinds`.
- [ ] Repeat adding `--pick-probe 100,100`: no crash, and **no cyan ring in the
      window**. An unset `hoverPixel` defaulting to (0,0) was one scene-layout
      change away from compositing an outline into every capture.

## G. Determinism, re-confirmed on real hardware

- [ ] Two identical offscreen runs, `fc /b a.png b.png` -> "no differences".
- [ ] A settled run at `--frames 30 --settle 30` reports `postBound: true`.
      Observed 3/3 here; the "25/25" figure rests on one agent's word.
- [ ] **Cross-machine reproducibility is NOT established.** `BuildDefaultLayout`
      sizes off `GetMainViewport()->WorkSize` - the hidden window's pixel size -
      so a different-DPI machine changes both the layout and the capture extent
      even with the pin held. Only same-machine determinism is proven.

## H. The dormant driver fault (spec phase 2)

- [ ] With **Parsec active**, run the windowed `[gpu]` suite three consecutive
      times: `cd bin\Debug-windows-x86_64-md\ArcaneTests` then
      `.\ArcaneTests.exe "[gpu]" -d yes`.
- [ ] `powershell -File scripts\check-faults.ps1 -Days 1`.
- [ ] **A negative result is valid and expected.** Ground truth: 7
      `nvwgf2umx.dll +0x1203ce` faults, all 2026-07-09 21:51 to 07-10 11:56, zero
      since. Outcome is either "reproduced, trigger characterised" or "not
      reproduced in three runs, recorded dormant, detector in place".

## H2. Precondition for ANY Dist run - do not skip

- [ ] `msbuild ReferenceProject.slnx /p:Configuration=Dist` **before** any Dist
      offscreen run. `ReferenceProject.slnx` is a **separate solution the engine
      build never drives**, so `ReferenceGame.dll` goes stale on a config flip and
      the host fails with "plugin: initial load failed". This is the known
      single-slot `Binaries/` + config-flip failure mode; it cost a debugging
      round during execution and is documented nowhere else.

## I. Things that CANNOT be closed at the desk - named so they are not assumed

- **The GPU-stall watchdog is armed but has never been observed firing.**
  Proving it needs a *stall*; `--crash-gpu` fires a TDR (device-removed), a
  different failure. **No stall trigger exists in the codebase.**
- **An offscreen editor run can hang with no diagnostics** - `PresentChromeFrame`
  returns false on `Skipped`, so `m_frameCount` never advances and `--frames N`
  never completes; no `Skipped`-streak bailout, watchdog disarmed on that host.
  Narrow (offscreen has no swapchain, so only a zero-sized surface could trigger
  it, which `CreateOffscreen` refuses at boot) but it is the one place the
  rationale and the code disagree.
- **Mesh picking is unimplemented.** `CollectPickables` builds only
  `View<WorldTransform,SpriteRenderer>` and `View<Collider2D,PhysicsBodyRef>`.
  Picking a 3D mesh silently returns the 2D sprite behind it. Mitigated by
  reporting `pickableKinds`/`meshesNotPickable`, not fixed.
- **`ShaderCompiler::IsIdle()`'s non-`_WIN32` stub returns `true`
  unconditionally**, so on the Linux port `--settle` silently degrades to the
  pre-fix stability-only predicate **with no warning**.


---

# DESK RESULTS — 2026-08-24 (Task 14, partial)

Run by the user at the desk with the controller interpreting. **These are
measurements, not estimates.** Everything below is `ReferenceProject`, 1280x720,
Debug, on the RTX 3070 with Parsec active.

## What was closed

| Item | Result |
|---|---|
| Runtime windowed regression | **PASS** - `--frames 5` and `--frames 120`, exit 0, `RenderErrorCount 0 -> 0` |
| Editor windowed smoke | **PASS** - **121,494 frames**, `0 error(s) latched` on every alive line, both contexts live (windowed chrome `format=11` + offscreen viewport `format=9`), clean exit |
| Fault log after windowed work | **CLEAN** - no `nvwgf2umx`, nothing new |
| `git status ReferenceProject/` after an editor session | **CLEAN** - the `Saved/*` + `!Saved/verify-layout.ini` ignore form holds |
| Spurious `gpu-stall` during a drag-storm | **NONE** - only 2026-08-12 diagnostics remain |
| Parity, offscreen vs windowed | **MEASURED, both backends** - see below |

## >>> THE MEASURED PARITY TOLERANCE — this is what Plan B's threshold comes from <<<

All four comparisons at matched census state (`spriteBound`/`meshBound`/
`postBound` all equal). Offscreen used `--frames 60 --settle 30`; windowed used
`--frames 120` (settle requires `--offscreen`).

| Comparison | Differing px | Max delta | p50 | p90 | p99 |
|---|---|---|---|---|---|
| **dx12** offscreen vs windowed | 36,288 (3.938%) | **21** | 0 | 0 | 19 |
| **vulkan** offscreen vs windowed | **36,288** (3.938%) | **8** | 0 | 0 | 7 |
| offscreen **dx12 vs vulkan** | **121** (0.013%) | 241 | 0 | 0 | 0 |
| windowed dx12 vs vulkan | 36,409 (3.951%) | 241 | 0 | 0 | 12 |

**Three things this establishes:**

1. **Offscreen renders the same scene as windowed.** ~96% bit-identical, and the
   differing ~4% is **the same 36,288-pixel set on both backends** - so it is the
   `format=9` vs `format=11` conversion, structural, not noise.
2. **The effects decompose exactly**: 36,288 (format) + 121 (backend) = 36,409
   (both). Independent and disjoint.
3. **The 121 cross-backend outliers are TEXT, not rendering.** They sit in
   x 131-872, y 89-241 - the ImGui overlay - and the values are binary,
   `(16,14,16)` vs `(255,255,255)`, flipping **in both directions**. A glyph
   landing one pixel differently where the two backends round oppositely.
   Nothing in the scene's shading, lighting or geometry differs.

**Design consequence for Plan B, free of charge:** a comparator with a
**per-pixel tolerance PLUS a `maxDiffPixels` count** passes this cleanly, while a
single global threshold either flags 121 legitimate text pixels or must be
loosened until it stops catching real regressions. That is exactly the two-knob
structure Playwright arrived at, reached here independently from our own data.

## >>> THE PRECONDITION THAT MAKES OR BREAKS THE COMPARISON <<<

**Both runs must reach the same CENSUS STATE before their images are
comparable** - not the same extent, not the same frame count. `spriteBound`,
`meshBound` **and** `postBound` must all match.

Worked example, from this session, same build, one variable changed:

| Windowed frames | postBound | Result vs the same offscreen image |
|---|---|---|
| 60 | **false** | **99.632% of pixels differ, max delta 72** |
| 120 | **true** | **3.938% differ, max delta 21** |

At 60 frames the windowed run had not yet bound the post chain, so an ungraded
image was being compared against a purple-graded one. It looks like catastrophic
failure and is **entirely an artifact**. The post chain binds later than
everything else because it compiles more shader permutations.

**Section A of the checklist is understated as written** - "capture at the same
extent and compare" is not sufficient. Check the census first, every time.

## Still open after this session

- **The layout seed (section D).** Unauthored, and the committed placeholder is
  **inert** - zero of its entries name a window this editor submits, so
  `LoadIniSettingsFromDisk` has never applied a setting. Layout pinning is
  **proven; seeding is not.** Must be authored *visibly different* from
  `BuildDefaultLayout` or the test cannot fail.
- **The driver reproduction attempt (section H).** Three windowed `[gpu]` runs
  with Parsec active, then `check-faults.ps1 -Days 1`. A negative result closes
  it validly.
- Completeness only: `--nri-graph --frames 1`, the windowed pick probe,
  offscreen project-switch / document-open, cross-machine DPI, Release/Dist
  windowed.
- **Section I is permanently open** and not desk-closable: no stall trigger
  exists for the watchdog; mesh picking is unimplemented; `IsIdle()`'s
  non-`_WIN32` stub returns `true` unconditionally.
