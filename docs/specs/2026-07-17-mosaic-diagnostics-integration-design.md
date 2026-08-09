# Mosaic Diagnostics Integration — Arcane as the Final Consumer

Date: 2026-07-17
Status: Design (approved)
Repos touched: Aphelyon (Arcane), Manifold2D (standalone), Astra (standalone)

## Goal

Make Arcane — the process that brings Mosaic, Manifold2D, and Astra together — the
single owner of their diagnostics. Mosaic ships logging and assert SEAMS but installs
no backend by design (zero dependencies); with nothing installed, every Mosaic/
Manifold2D/Astra `MOSAIC_LOG_*` call is dropped silently. Arcane installs one log
sink + one assert handler that forward into its existing engine logger, so physics/
ECS/core diagnostics land in one stream. Then Manifold2D actually emits useful logs
(a site-map), and Astra is re-vendored so its diagnostics flow through the same seam.

This finishes three standing "Mosaic loose ends":
1. The Manifold2D assert migration (done + green in the standalone repo, uncommitted,
   not re-vendored — Aphelyon's vendored copy still uses `<cassert>`).
2. The Manifold2D logging site-map (proposed, not built).
3. Arcane wiring itself as the Mosaic sink (parked).

## Background: the Mosaic seam (unchanged by this work)

`Mosaic/Log.hpp` and `Mosaic/Assert.hpp` expose fn-ptr seams:
- `Mosaic::SetLogSink(LogSink, void*)` — `LogSink = void(*)(const LogRecord&, void*) noexcept`.
  `LogRecord { LogLevel level; string_view category; string_view message; source_location }`.
  Messages are **plain strings** (no fmt in a zero-dep core). Compile floor
  `MOSAIC_ACTIVE_LEVEL` + runtime `SetLogLevel`.
- `Mosaic::SetAssertHandler(AssertHandler, void*)` — `AssertHandler = AssertAction(*)(const AssertContext&, void*) noexcept`.
  `AssertContext { const char* expression; const char* message; source_location }`
  (no category field). Default handler routes through the log sink at Critical (or
  stderr), then `FailFatal` does break-if-debugger + abort.

**Two hard constraints that shape the design:**
- **Zero-dependency invariant:** Mosaic must not reference Arcane/spdlog
  (`rg 'Arcane/|Astra/|Manifold2D/' include src` stays empty). So the record→spdlog
  ADAPTER is irreducibly Arcane-side; nothing moves into Mosaic. This work makes
  **no Mosaic edit**.
- **Per-module storage:** `g_logSink`/`g_assertHandler` are inline atomics compiled
  separately into each statically-vendored module (Arcane.dll, Sandbox.dll, Loom.exe,
  ArcaneTests.exe). One install does not cover all modules — each module installs into
  its own copy. This mirrors how the engine already adopts the ImGui context, Astra
  `TypeContext`, and the Vulkan dispatcher per-module.

---

## Sub-project A — Diagnostics integration (Arcane)

The integration lives in Arcane's existing diagnostics homes, NOT a new `MosaicBridge`.
Arcane already has `Base/Log.{hpp,cpp}` (the `ARCANE_API` engine logger `"Arcane"`,
`ARC_*` macros, spdlog owned in Arcane.dll, reached via `Arcane::Log::Engine()`). We
extend it and add a parallel `Base/Assert.{hpp,cpp}`.

### Routing decision: one engine logger, category as text

All seam output flows through the single `Arcane::Log::Engine()` logger. There is **no
"Mosaic" logger and no per-category logger** — "Mosaic" is the transport, not a source.
- **Logs:** `Engine()->log(loc, level, "[{}] {}", record.category, record.message)`.
  The category (`"Manifold2D.Broadphase"`, `"Astra"`, …) is set by the *source*
  library's `MOSAIC_LOG_CATEGORY` and rides in the message text. `record.message` is
  passed as a fmt ARGUMENT (literal `"[{}] {}"` format), so a stray `{}` in a message
  can't fmt-inject.
- **Asserts:** `Engine()->log(loc, critical, "assertion failed: {}{}", expr, msg-or-empty)`.
  `AssertContext` has no category; the stringized condition + `source_location`
  (file:line) are the attribution — no "Mosaic" bucket needed.

Both preserve `source_location` by passing `spdlog::source_loc{file, line, func}` so
file:line is accurate even though the call originates inside the adapter.

Because `Arcane::Log::Engine()` is `ARCANE_API` (the single Arcane.dll spdlog
instance), every module's sink funnels into ONE logger regardless of the calling
module — the single-instance property falls out of the existing design.

### `Base/Log.{hpp,cpp}` (extend)

- `Log.cpp` (Arcane.dll): file-local `MosaicLogSinkImpl(const Mosaic::LogRecord&, void*) noexcept`
  (level map + the routing above, whole body in `try{}catch(...){}` — the sink is
  `noexcept` and must not throw or re-enter Mosaic). Plus
  `ARCANE_API Mosaic::LogSink Arcane::Log::MosaicSink() noexcept { return &MosaicLogSinkImpl; }`.
- `Log.hpp`: `#include <Mosaic/Log.hpp>`; declare `ARCANE_API Mosaic::LogSink MosaicSink() noexcept;`
  and add an **inline** installer beside the `ARC_*` macros:
  ```cpp
  inline void InstallMosaicSink() noexcept { Mosaic::SetLogSink(MosaicSink(), nullptr); }
  ```
  Inline is load-bearing: called from module X, the inline `Mosaic::SetLogSink` writes
  X's storage, pointing at Arcane.dll's adapter.

### `Base/Assert.{hpp,cpp}` (new — parallel to Log)

- `Assert.cpp` (Arcane.dll): file-local `MosaicAssertHandlerImpl(const Mosaic::AssertContext&, void*) noexcept`
  (routes Critical through `Arcane::Log::Engine()` as above, returns `AssertAction::Break`
  so Mosaic's `FailFatal` still breaks-if-debugger + aborts). Plus
  `ARCANE_API Mosaic::AssertHandler Arcane::Assert::MosaicHandler() noexcept`.
- `Assert.hpp`: `#include <Mosaic/Assert.hpp>`; declare the accessor; add
  ```cpp
  inline void InstallMosaicHandler() noexcept { Mosaic::SetAssertHandler(MosaicHandler(), nullptr); }
  ```
  **and** first-class Arcane assert macros (the parallel to `ARC_*` log macros — the
  "define its asserts" half), thin wrappers so engine code gets ergonomic guards that
  flow through the installed handler:
  ```cpp
  #define ARC_ASSERT(cond, msg)  MOSAIC_ASSERT(cond, msg)
  #define ARC_VERIFY(cond, msg)  MOSAIC_VERIFY(cond, msg)
  #define ARC_ENSURE(cond, msg)  MOSAIC_ENSURE(cond, msg)
  ```

### Install sites (one call-pair per module, in that module's own code)

`Arcane::Log::InstallMosaicSink(); Arcane::Assert::InstallMosaicHandler();` at:
- **Arcane.dll** — the `Runtime` ctor (Arcane.dll code → installs Arcane.dll's copy).
- **Loom.exe** — `main`, after `Arcane::Log::Init` / before loading the plugin.
- **Sandbox.dll** (+ **PlaygroundGame.dll**) — `GamePlugin_Init`.
- **ArcaneTests.exe** — the diag test installs it locally (see Testing); the suite
  otherwise leaves the default (stderr) so an unexpected assert is still visible.

No `EngineContext`/plugin-ABI change: the plugin links Arcane.dll and calls the
`ARCANE_API` accessors directly; only the inline `SetLogSink`/`SetAssertHandler` need
to run in the plugin, which they do because the installer is inline.

---

## Sub-project B — Manifold2D diagnostics content (standalone repo → re-vendor)

All edits in `D:\dev\starworks\Manifold2D`, then re-vendored into
`ThirdParty/Manifold2D` in one refresh.

### b1 — commit the assert migration
The 7-file `assert()` → `MOSAIC_ASSERT` migration is done + green there but uncommitted.
Commit it (sibling-repo convention: no AI trailers). Files: `Contact.hpp`,
`ContactConstraintSimd.hpp`, `DynamicTree.cpp`, `TileGrid.cpp`, `ConstraintGraph.cpp`,
`PhysicsWorld.cpp`, `SoftStep.cpp`.

### b2 — the logging site-map
Per-subsystem category via `#define MOSAIC_LOG_CATEGORY "Manifold2D.<Subsystem>"` at the
top of each `.cpp` (before `#include <Mosaic/Log.hpp>`): `Broadphase` (DynamicTree,
SpatialGrid/Hash, TileGrid), `Solver` (SoftStep, ContactConstraint), `World`
(PhysicsWorld), `Graph` (ConstraintGraph), `Contact`.

Philosophy: silent hot path (determinism + perf), diagnose at the API boundary.
- **WARN** — the ~12 `IsValid`-guarded mutators that today silently no-op on a stale
  `BodyHandle` (`SetPosition`/`SetVelocity`/`ApplyForce`/… ): log that the op on a
  stale/invalid handle was ignored (surfaces game-code use-after-free).
- **WARN** — SaneBox non-finite-AABB drops (`SpatialHash.cpp:112`, `SpatialGrid`).
- **DEBUG** — capacity `Grow`/`EnsureCapacity`.
- **TRACE** — body/fixture/joint lifecycle (Add/Remove).
- **INFO** — exactly ONE: the world-create banner (config + SIMD backend) in the
  `PhysicsWorld` ctor.
- **NEVER log** designed clamps that fire in normal motion (velocity cap
  `SoftStep.cpp:366-381`, friction/motor `Joints.cpp:312/395`).

### b2 (guards) — input-validation ADDS (behavior change, not just logs)
New guards at the API boundary that both REJECT the bad input AND WARN: non-finite
(NaN/inf) position/velocity at `AddBody`/`SetPosition`/`SetVelocity`; degenerate shape
at `AddFixture`. These change behavior (a bad body/fixture is rejected instead of
poisoning the sim), so they get tests in the standalone Manifold2D Catch2 suite.

### Constraint: messages are plain string literals
No runtime values in a message (zero-dep seam has no fmt). Logs are categorical +
`file:line` (e.g. `"stale BodyHandle mutation ignored"` at `PhysicsWorld.cpp:NNN`), not
`"body 5 at NaN"`. Accepted — richer formatting is the host's business.

Re-vendor b1 + b2 together via the Manifold2D `scripts/vendor.ps1` (which already
excludes `ThirdParty` so no nested Mosaic). The vendored copy stops being stale.

---

## Sub-project C — Astra routes through Mosaic (last, gated)

Aphelyon's vendored `ThirdParty/Astra` predates Astra's Mosaic adoption (zero Mosaic
usage). To make Astra's `ASTRA_LOG_*`/asserts flow through the sink:
1. In `D:\dev\starworks\Astra`: commit the Mosaic-adoption shims (the ~2 dirty files —
   `Core/Log.hpp`/`Core/Assert.hpp` re-exporting Mosaic's, per the host-adoption pass).
   Verify AstraTest green first.
2. Re-vendor Astra's `include/` into Aphelyon `ThirdParty/Astra` (Astra is header-only).
   Review the diff — a re-vendor pulls ALL Astra changes since the last vendor, not just
   the Mosaic adoption; confirm nothing unexpected rides along.
3. Ensure Mosaic is on the include path for every Aphelyon project that compiles Astra
   TUs (already wired via `IncludeDir.Mosaic`); Astra's shims `#include <Mosaic/Log.hpp>`.
4. Full engine rebuild + `~[gpu]` gate.

Highest risk (Astra is the ECS backbone; whole-engine rebuild). Sequenced last behind
its own gate. Value is architectural (unified diagnostics ownership) more than log
volume — Astra emits comparatively little.

---

## Sequencing & gates

A → B → C. Each ends green on Debug `~[gpu]` (and Release for C's whole-engine rebuild).
- After A: the pipe exists; testable by forcing a Manifold2D `MOSAIC_ENSURE` / emitting a
  `MOSAIC_LOG_*` and seeing it in the engine logger.
- After B: physics diagnostics flow; Manifold2D's own standalone gate green (incl. the new
  input-guard tests).
- After C: Astra diagnostics flow; AstraTest green + Aphelyon `~[gpu]` green.

## Testing

- **A (`[diag]`, CPU-only):** install a capturing spdlog sink on the `"Arcane"` logger,
  `Arcane::Log::InstallMosaicSink()`, emit a `MOSAIC_LOG_WARN("...")`, assert the captured
  line contains the category + message. Assert path: `Arcane::Assert::InstallMosaicHandler()`
  + a `MOSAIC_ENSURE(false, "...")` (non-fatal) routes a Critical line through and returns
  false. Level-map unit (Mosaic→spdlog). Adapter is `noexcept` — a throwing capture sink
  must not escape.
- **B:** the standalone Manifold2D Catch2 gate stays green; ADD tests for the input-
  validation guards (NaN pos/vel rejected at `AddBody`/`SetPosition`; degenerate fixture
  rejected). Logging sites don't change behavior (determinism-safe, like the asserts).
- **C:** AstraTest green in the sibling repo; Aphelyon Debug + Release `~[gpu]` green.

## Out of scope
- Making Mosaic a shared library (would make one install global but changes the vendoring
  model for all consumers — not needed here).
- Value-rich (fmt) log messages through the seam.
- An OSD/on-screen fail-fast for asserts (possible later; the handler returns `Break`
  which already aborts).
- Structured/JSON diagnostics routing.
