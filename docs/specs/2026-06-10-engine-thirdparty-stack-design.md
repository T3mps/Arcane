# Engine Third-Party Library Stack — Design

**Date:** 2026-06-10
**Status:** APPROVED — design contract for the unified C++ engine's dependency stack.
**Context:** The decision to build a full C++23 engine (unified client + editor solution,
game-as-DLL hosted by Editor/Runner) was made 2026-06-10 on top of the port scoping study
(`docs/superpowers/audits/2026-06-10-cpp-client-port-scoping.md`). Strangler migration:
the LÖVE client stays alive as reference implementation/oracle; no hand-coded Lua screen
is ever ported (the JSON UI runtime is the UI port). This spec decides the third-party
libraries, how each arrives in the build, what was rejected and why, and how the stack
is verified. The engine/editor *project architecture* (module split, game-DLL ABI,
hot-reload sequence) is the NEXT design conversation, not this one.

## Constraints (decided)

- **Language/toolchains:** C++23, native MSVC (VS2026) + g++ — premake5 `vs2026` and
  `gmake2` actions. Windows + Linux day one; macOS later via MoltenVK; mobile/console
  future.
- **Rendering:** NVRHI (user mandate), **D3D12 + Vulkan backends only** (D3D11 off).
  NVRHI has no Metal backend; macOS = Vulkan-through-MoltenVK, deferred.
- **Shaders:** HLSL single-source → DXC → DXIL + SPIR-V. The 33 client GLSL files are
  rewritten in HLSL once.
- **No scripting VM.** Gameplay is C++ in the game DLL. Designer scripting is the
  BehaviorGraph (C++ interpreter). Iteration = DLL hot reload + JSON hot reload.
- **Build:** premake5 everywhere (matches Server, Tools, Astra). vcpkg only for
  deep-build-system deps (existing rule; SDL3 qualifies, like libpq).
- **Vendor preference:** ThirdParty/ premake wrappers in the Catch2/rapidcheck pattern.

## The stack

| Role | Pick | Arrives via | License |
|---|---|---|---|
| GPU abstraction | NVRHI (DX12+VK; DX11 off) | Vendored + premake wrapper; Vulkan-Headers + DirectX-Headers vendored include-only | MIT |
| Shader pipeline | DXC + ShaderMake | Prebuilt tool binaries in `ThirdParty/tools/`, versions pinned | MIT/NCSA |
| Platform layer | SDL3 (window/events/input/gamepad/IME/DPI) | vcpkg, static, existing v143 overlay triplet | zlib |
| ECS / scene | **Astra** (in-house) | Vendored via subtree from its own repo (no `bin/`/`ide/` artifacts) | MIT |
| Job system | enkiTS | Vendored + premake wrapper | zlib |
| Audio | miniaudio | Vendored (single header) | MIT-0 |
| Text | FreeType + first-party skyline atlas (port of the tested Lua packer) | Vendored + premake wrapper | FTL |
| Math | glm | Vendored (header-only) | MIT |
| Images | stb_image (+ stb_image_write for tools) | Vendored (header-only) | MIT/PD |
| JSON | nlohmann/json | Already vendored | MIT |
| Logging | spdlog | Already vendored | MIT |
| Networking | `Server/Common` (TcpSocket, Protocol, Types, Crypto, RateLimiter) | First-party static lib, referenced not copied | — |
| Editor UI | Dear ImGui + imgui-node-editor | Already vendored | MIT |
| Game UI | First-party JSON UI runtime (21 element types from UIEditor) | First-party | — |
| RNG | XoshiroCpp | Already vendored | MIT |
| Profiling | Tracy client | Vendored + premake wrapper; viewer prebuilt in `ThirdParty/tools/` | BSD-3 |
| GPU crash debug | NVIDIA Aftermath | Optional, Debug configs, `NVRHI_WITH_AFTERMATH` | NVIDIA EULA |
| Testing | Catch2 + rapidcheck | Already vendored | BSL/BSD |

### Astra adoption conditions (decided after full review, 2026-06-10)

Astra was reviewed in depth: builds clean on VS2026, **455/455 tests pass** (28 suites),
benchmarks corroborate claims (~1 ns/entity single-component iteration; 2.5–3 ns
two-component; creation 4–7 M entities/s). Verdict: production-grade, adopted as a
first-class engine pillar over EnTT (no scheduler, bare-bones meta, sparse-set
trade-offs) and flecs (C core, archetype fragmentation from relationship pairs,
feature overlap we'd own anyway). Astra's reflection-with-attributes + signals +
versioned serialization are better-fitted to the editor than either.

**The Astra hardening workstream is a booked prerequisite** (its own milestone, in the
Astra repo, pulled in via subtree):

1. **enkiTS-backed executors** — `ParallelForEach` and `ISystemExecutor` currently
   spawn ad-hoc threads; measured wall-clock at 1M entities is 2.43 ms parallel vs
   1.30 ms sequential (thread-spawn overhead). Persistent worker pool fixes it.
2. **CI matrix (MSVC + gcc + clang)** — the g++ claim is currently unverified; this
   gates the engine's Linux build.
3. **Doc reconciliation** — README documents `config.threadSafe` (doesn't exist),
   "components must be trivially copyable" (stale; descriptors handle non-trivial
   types), CLAUDE.md path drift.
4. **FlatMap/FlatSet fuzzing** — highest-risk hand-rolled containers (~1.8K LOC
   Swiss tables); fuzz pass for cheap confidence.
5. **TypeContext injection (cross-DLL identity)** — `ComponentID` comes from
   per-module inline-static atomic counters; engine EXE and game DLL would assign
   different IDs to the same type. Fix: process-wide `TypeContext`
   (`CreateTypeContext`/`SetTypeContext`/`GetTypeContext`), assignment keyed by the
   existing stable XXHash64 type-name hash, per-module static caching keeps the hot
   path free. `MetaRegistry::Instance()` folds into the same context. Plus re-entrant
   `ReRegisterComponent` so descriptors' function pointers can be refreshed after a
   game-DLL reload. Items 5 (and 1) must land **before** the engine/game module
   boundary is designed — they shape the plugin ABI. The blessed hot-reload sequence
   is: serialize world → unload DLL → load new DLL → re-register → deserialize
   (Astra's versioned serialization with min-version migration handles component
   layout changes between reloads).

## Rejected / deferred (and why)

- **Box2D** — rejected. The in-house physics engine (M1–M4) keeps its hash-identical
  determinism oracle, TileGrid integration, and tuned movement feel; Box2D was already
  removed from the client once. The port targets oracle parity with the Lua reference.
- **shaderc** — rejected. GLSL→SPIR-V only; strands the D3D12 backend. DXC emits both
  targets from HLSL.
- **VulkanMemoryAllocator** — not needed. NVRHI owns device-memory allocation behind
  its resource API.
- **assimp** — rejected. No 3D model pipeline exists or is planned (isometric sprites).
- **GLFW** — rejected for SDL3. No mobile path, no IME, shallow gamepad/haptics. For a
  gacha title, mobile is the genre's center of gravity; SDL3 carries through.
- **EnTT / flecs** — passed over for Astra (see adoption rationale above).
- **Taskflow** — passed over for enkiTS; heavyweight, and its graph power overlaps
  Astra's scheduler.
- **HarfBuzz + ICU** — deferred until RTL/Indic localization is a business reality.
  Latin + CJK need no complex shaping; FreeType + atlas covers them.
- **LuaJIT embedding** — rejected (no scripting VM; see constraints).
- **PhysFS / pack formats / zstd asset compression** — deferred to the asset-pipeline
  phase. `std::filesystem` + loose files now, matching DataStore conventions.
- **SDL3 renderer / SDL_GPU / audio subsystems** — unused (NVRHI and miniaudio own
  those jobs). Note: the stock vcpkg port has no per-subsystem compile-out; static
  linking + linker GC discards unreferenced objects, which is most of the benefit. A
  trimmed overlay port is a later nicety.

## Integration mechanics

- **Workspace:** new premake workspace; first-party projects `Engine` (static lib),
  `Game` (DLL), `Runner` (exe), `Editor` (exe), `Playground` (exe, see Verification).
  Configurations Debug/Release/Dist mirroring Astra and the Server's outputdir pattern.
  Detailed module architecture is the next design.
- **NVRHI wrapper:** one static lib, `NVRHI_WITH_DX12=1`, `NVRHI_WITH_VULKAN=1`,
  `NVRHI_WITH_DX11=0`; Linux filter compiles the Vulkan backend only.
- **Shader build:** premake pre-build step invokes ShaderMake/DXC over
  `data/shaders/*.hlsl` → `dxil/` + `spirv/` output trees. Shaders are data: same
  hot-reload philosophy as the JSON.
- **Tracy:** compiled in only where `TRACY_ENABLE` is defined (Debug + a
  Release-Profile flavor); zero cost in Dist.
- **Astra:** headers consumed directly; its test suite runs in its own repo and gates
  subtree pulls.
- **Floating-point policy (determinism-load-bearing):** engine-wide `/fp:precise`
  (MSVC) and no `-ffast-math` (g++); FMA contraction disabled
  (`-ffp-contract=off` / MSVC default under precise) in the physics project. Astra's
  own benchmark scripts use `/fp:fast` — that must not leak into engine builds.
  Determinism gate is per-platform self-consistency (matching the existing physics
  harness model); cross-compiler bit-parity is a stretch goal.

## Risks

| # | Risk | Mitigation / fallback |
|---|---|---|
| R1 | NVRHI premake wrapper drifts on upstream pulls | Wrapper is small; review diffs. Plan B: CMake-prebuild NVRHI into `ThirdParty/installed/`, link binaries |
| R2 | SDL3 vcpkg port friction with v143 overlay triplet | Plan B: CMake-build SDL3 once, vendor binaries (love2d/premake precedent) |
| R3 | Astra never compiled by g++ | Hardening item 2 (CI matrix) lands before the Linux build is stood up |
| R4 | GLSL→HLSL rewrites have no oracle | Accepted: RenderDoc captures + side-by-side with the live LÖVE client (kept as reference) |
| R5 | MoltenVK gaps at the macOS milestone | Deferred by design; audit NVRHI-required features vs portability subset then |
| R6 | Cross-compiler float determinism | Per-platform self-consistency is the gate; parity is stretch |
| R7 | Tool binaries bloat repo (DXC tens of MB) | Accepted (love2d precedent); versions pinned, licenses recorded |

## Verification — the Playground

Not a dry smoke test: a single cohesive sample (`Playground` project) where every
dependency works *together*, kept green as the stack evolves. Nothing in it is
throwaway — its pieces graduate into the engine.

**The scene:** balls bouncing and ricocheting inside the window. Each ball is an
**Astra** entity (Position/Velocity/Radius/Color components); movement + collision
systems are registered with Astra's **SystemScheduler** and executed on **enkiTS**
workers via the new executor (hardening item 1's first consumer). Every
collision plays a **miniaudio** tone (pitch scaled by impact speed). Ball sprites
load through **stb_image**; HUD text (entity count, frame ms, backend name) renders
through **FreeType** + the first-party atlas. Math is **glm**. Frame and system zones
are instrumented with **Tracy**.

**The ImGui layer** (rendered through a first-party `imgui_impl_nvrhi` backend — a
real deliverable the Editor needs anyway, on `imgui_impl_sdl3` for platform events):
- **Runtime GPU-backend swap** (D3D12 ↔ Vulkan dropdown): full device/swapchain/
  resource teardown and recreation. This deliberately forces the engine's
  resource-recreation path to exist from day one — if backend swap works, device-lost
  recovery and settings changes are already architecturally solved.
- Spawn/despawn controls (batch-create stress), pause/step, tone toggle, Tracy
  connect hint.

**Initial collision is a toy** (circle-vs-circle + walls, ~100 lines). When the
in-house physics port lands (oracle-gated), the Playground swaps its toy collision
for the real engine — becoming the physics port's first visual consumer and the
migration's proof-of-life.

**Gate:** Playground builds and runs on MSVC day one; the same target must pass under
gmake2/g++ when the Linux build is stood up. Plus `THIRDPARTY-LICENSES.md` recording
license + pinned version per dependency.

## Deferred to the next design conversation

- Engine/Editor/Game/Runner module architecture, the game-DLL C ABI, and the
  hot-reload sequence (depends on Astra hardening items 1 & 5).
- Asset pipeline (pack format, compression, cooking) and how `services.Assets`
  semantics port.
- Render-graph/pass architecture on NVRHI (how `systems/render`'s Pipeline design
  re-hosts).
- Combat Sphere and LevelEditor build directly on the C++ engine (decided direction);
  their designs follow the architecture conversation.
