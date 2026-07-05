# Arcane Engine — Gap Survey (2026-07-04)

Read-only survey of the Arcane C++ engine across three lenses: **known-issues triage**,
**systematic subsystem sweep** (5 parallel read-only agents), and **roadmap-level gaps**.
Physics-the-subsystem was excluded from the code sweep (actively worked, well-mapped);
its already-flagged engine gaps are triaged separately at the end.

Severity = likelihood/impact of biting. Effort = S/M/L. "Verified" = the controller
re-read the actual code path; "reported" = flagged by a sweep agent, not independently
re-confirmed. Line numbers drift — locate by content.

---

## Executive summary

Two shapes stand out.

**Strategically**, the engine is deep in one dimension and absent in most others a
playable slice needs. The physics stack has run ~6 weeks past M6's original scope
(MT scaling, SIMD, MKS units, island-split — 25+ plans) and is the most mature,
most over-engineered subsystem — against a game with **zero consumers of it yet**.
Meanwhile M7 (Grimoire editor) has not started, and the game layer — a Game.dll, an
engine-side UI runtime, combat, client-side netcode wiring — does not exist. The single
largest content-blocking gap is the UI runtime (32.7K LOC of Lua UI in the client, no
engine equivalent, no test oracle).

**At the code level**, the non-physics engine is unusually clean of marked debt
(essentially zero TODO/FIXME/HACK across `Arcane/Arcane/src` + `Arcane/Loom/src`), so
the real gaps are *structural*, not comment-flagged. They cluster in three places: a
handful of confirmed latent bugs, a weak persistence/serialization spine, and thin
security/test coverage in Core.

---

## A. Confirmed latent bugs (verified; fix-worthy independent of roadmap)

These are real defects, mostly small fixes. "Latent" = currently masked because the
production consumers (Sandbox/tests) happen not to hit the path, but any new consumer
will.

1. **Runtime Assets facade is permanently null-device.** *(verified — high — M)*
   `Assets::Create(nullptr)` at `Runtime.cpp:78`; `SetRenderResources` (`Runtime.cpp:150-154`)
   only stores `device`/`shaders` on the impl and never rewires the facade. Any
   `Runtime().AssetsFacade().GetTexture()` dereferences null `m_device` at `Assets.cpp:125`
   and crashes. The `:49` comment ("Null until the host calls SetRenderResources") promises
   wiring the code doesn't perform. Fix: rebuild/rebind the facade's device inside
   `SetRenderResources`, or lazily bind on first use.

2. **Registry snapshot/save silently drops all resources.** *(verified — high — M)*
   Astra `Registry::Save()` (`ThirdParty/Astra/.../Registry.hpp:1417-1422`) serializes
   entities, archetypes, and the relationship graph but **not** `m_resourceStorage`. So
   `SnapshotRegistry`/`RestoreRegistry` (hot-reload) and binary scene save/load lose every
   `SetResource` value (SceneRoot, TextureTable, RenderContext2D, …). Plugins survive only
   by hand-persisting SceneRoot (`PlaygroundGame.cpp:132-157`); any other resource vanishes
   across reload. Fix is upstream in Astra (serialize resource storage) or an engine-side
   resource-persistence pass.

3. **`SnapshotRegistry` swallows a Save failure into an empty snapshot.** *(verified — med — S)*
   `Runtime.cpp:170-171`: `return r.IsOk() ? … : std::vector<std::byte>{}`. A real save
   failure surfaces only as a generic downstream "reload failed → rollback" with the root
   cause logged nowhere. Fix: propagate the error.

4. **Audio leaks every fire-and-forget voice.** *(verified — high — M)*
   No `ma_sound` end-callback, no `IsPlaying`/at-end query, no reclaim anywhere in `Audio/`
   (`AudioDevice.cpp` Play path ~552-660). Non-looping voices keep `alive=true` and pin a
   `VoiceSlot` forever until an explicit Stop. Every one-shot SFX permanently leaks a slot.
   Fix: register an end-callback that frees the slot (+ expose a state query).

5. **AssetCache never evicts — unbounded growth.** *(verified — high — L)*
   `Put`/`PutFailure` are called (`Assets.cpp:104,113,131,147`) so memoization works, but
   the LRU/refcount/eviction path (`Acquire`/`Release`/`Evict`/`LeastRecentKey`,
   `AssetCache.hpp:74-114`) has no callers — no budget is ever enforced; texture/bytes/JSON
   caches grow without bound. Fix: wire a budget + eviction, or remove the dead LRU surface
   if unbounded is intended.

6. **Rollback-of-a-failed-rollback yields a silently dead plugin.** *(reported — med — M)*
   `PluginHost.cpp:247-263`: if the new image fails AND re-`Init` of `previous` also fails,
   `current` is still assigned though its `plugin` is empty; `IsLoaded()` → false, `Vtable()`
   → null, and the main loop's `if (vt)` guards skip all FixedUpdate/Update/DrawUI — the game
   stops simulating while still rendering, only the earlier error logged. Fix: on double
   failure, surface an explicit dead-state rather than a half-assigned current.

7. **Scene-JSON reader throws uncaught in an exception-free engine.** *(reported — med — S)*
   `ReflectionJson.hpp:50-81` (`in.get<T>()`, `in[0..3]`) and `SceneSerializer.hpp:78-112`
   have no type/length guards or try-catch; a hand-edited/corrupt scene file throws an
   `nlohmann` exception through Result-typed code. Fix: guard + convert to a Result error.

---

## B. Code-level gaps by subsystem (structural, non-blocking-but-real)

### Core (Net/Crypto/Types/Util/Jobs/Geometry/Simd)

- **Crypto Linux-portability, high blast radius.** `/dev/urandom` read return unchecked
  (`Crypto.hpp:183`) → short read leaves salt/token tail zeroed with no error; weak
  `std::random_device` fallback (`Crypto.hpp:160-197`, self-noted DEFER L-V5-2) with no
  startup entropy self-test. Silent weak secrets the moment Core is lifted off Windows.
  *(high on Linux / low on Windows — M)*
- **Logger emits hand-built JSON without escaping** (`Logger.hpp:189-278`). Content-derived
  fields (item/banner names, ip, reason) with a `"`/`\`/control char break the JSON and
  permit log/field injection into the analytics stream. *(med — M)*
- **Protocol/Net validation soft spots**: MsgId loaded as int then `static_cast<MsgId>`
  (`Protocol.hpp:187,339`) — id > 65535 silently truncates; no dup/zero-id checks. Frame
  `'\n'` delimiter never verified before consuming (`TcpSocket.hpp:456`). `ConnectSocket`
  does no DNS (`inet_pton` only, `TcpSocket.hpp:253`) and ignores its return on an invalid
  literal. IPv4-only listener. *(low-med — S/M each)*
- **Geometry predicates are non-robust** (`Predicates.hpp:22`, exact `Cross != 0` compares).
  For `T=float`, near-collinear/large-coordinate inputs can mis-orient → wrong/non-canonical
  hulls. No adaptive/exact path (contrast Physics, which has robust predicates). *(med — L)*
- **BitSet::Set / FunctionRef::operator() unchecked** (`BitSet.hpp:36`, `FunctionRef.hpp:41`)
  — OOB write / null-thunk call are UB with no debug assert. *(low-med — S)*
- **SIMD NEON unvalidated by design** + rsqrt/recip precision divergence across backends
  (scalar full / AVX2 ~12-bit / NEON ~8-bit, no Newton refine). The `[simd]` tolerance tests
  are tuned to AVX2/scalar; NEON may not hold. *(med for the ARM port — M)*

### Renderer (device/batcher/text/tonemap/shaderlib/imgui)

*(The render graph, bindless, batcher-v2, materials, 2D lighting/GI, and post stack are
milestone-deferred to M3.5+/Later per the north-star spec — see Section D, not bugs.)*

- **Text atlas is a hard 1024×1024 wall** (`TextSystem.cpp:30,383-395`). On packer miss a
  glyph gets `hasInk=false` **permanently** (logged once); no second page/eviction. Highest
  in-scope renderer severity. *(high — M)*
- **Batcher2D has no blend modes** (`Batcher2D.cpp:401-406`; key omits a blend field,
  `:295-298`). Straight alpha only — additive/multiply glows/lights/particles are impossible
  on the mandated single path. Conflicts with the homogenized-rendering mandate. *(med — M)*
- **Batcher binding-set cache has no eviction / no RemoveTexture** (`Batcher2D.cpp:350-364`).
  A leak now; an ABA/UAF the moment dynamic asset textures land (a freed-then-realloc'd
  texture collides with a stale binding set). The ImGui backend already solved this
  (`ImGuiNvrhi.cpp:91-103`); the batcher has a "do in M2b" comment and no code. *(med, high
  once asset textures land — M)*
- **Thin device-lost / present handling.** D3D12 device-removed is logged (not counted, not
  recovered) then runs GC on a dead device (`DeviceD3D12.cpp:279-296`); VK `eSuboptimalKHR`
  return is `(void)`-cast so a stale/stretched surface persists (`DeviceVulkan.cpp:387-394`).
  VK **warning**-severity VUIDs aren't latched into `RenderErrorCount` (`DeviceVulkan.cpp:60-64`).
  *(low-med — S/M)*
- **No exposure in the HDR path** (`tonemap.hlsl:23-34`); ACES+2.2 operator hardcoded, no
  auto-exposure/operator selection/LUT/bloom. Single linear-clamp sampler in the batcher —
  no point sampling (pixel-art) or wrap/repeat (tilemaps). *(med — M)*
- **ImGui user-callback guarded only by `IM_ASSERT`** (`ImGuiNvrhi.cpp:234-236`) — compiled
  out under NDEBUG, so in Release a callback cmd (incl. ImGui's own reset sentinel) is drawn
  as garbage. *(med — S)*

### Runtime / Scene / Serialization

- **Persistence/serialization is the weakest real spine.** Beyond bugs #2/#3/#7: the
  reflection→JSON layer silently drops unsupported field types (mat3/mat4/quat/containers,
  `ReflectionJson.hpp:143`) and unresolved enums (`:120-131`); scene JSON has no
  schema/version, a hardcoded 2-component roster (LocalTransform+SpriteRenderer only), a
  single `parent` index, and drops non-hierarchical `AddLink` relations
  (`SceneSerializer.hpp:35-117`). Binary path is solid; JSON path is a proof-of-concept.
  This is the natural M7/Grimoire predecessor work (arbitrary component authoring). *(med — M)*
- **RunLoop has no pause / single-step / time-scale** (`RunLoop.hpp:23-102`). Notable
  because M7/Grimoire is *defined* as adding exactly this sim-time control — it's the M7
  prerequisite, not just a gap. Backlog-drop (accumulator zeroed at `maxStepsPerFrame`) is
  also silent (`RunLoop.hpp:45-46`). *(med — M)*
- **Owned-TypeContext dangling-slot** (`Runtime.cpp:113-120`): when `externalContext==null`,
  `~Impl` frees `ownedContext` but the module's installed slot still points at it. Latent
  (production always injects). *(low — M)*

### Platform / Input / Audio / Text

- **Exe-relative path resolution is `#ifdef _WIN32` only** (`Assets.cpp:30-35`,
  `InputActions.cpp:36-41`, `TextSystem.cpp:43-49`) — on non-Windows it silently falls back
  to CWD, breaking the documented contract. Single-window assumption in `PumpEvents`
  (global SDL queue) and Win32-only `NativeHandle()`. *(med — M)*
- **Input "Phase 6" surface absent**: no rebind API, glyphs, live-preview, or replay
  (`InputActions.hpp:8`). Only the first gamepad is sampled (no multi-pad/per-player,
  `InputDevices.cpp:187-197`); composite active-device attribution hardcoded to keyboard
  (`InputActions.cpp:918-921`); numeric parse errors swallowed (`catch(...){}`,
  `InputActions.cpp:239,389-408`). *(med — M/L)*
- **Audio is device-init only** beyond the leak (#4): `StreamFromDisk` isn't streaming
  (loads whole file to RAM, `AudioDevice.cpp:606-613`); no mixing bus depth beyond basic,
  spatialization always disabled (`:573`), no device-lost/underrun handling, no state query.
  *(med — M)*
- **Text has no real layout**: no word-wrap, HarfBuzz shaping, RTL/bidi, or ligatures; GPOS
  kerning ignored (kern-table only, `TextSystem.cpp:208-211`); missing glyph → silent
  zero-advance blank (no `.notdef`/tofu); no `UnloadFont` (fonts/glyph caches only grow).
  *(med — L)*

---

## C. Test-coverage holes (where a regression would land silent)

- **Security-critical, near-zero coverage**: `RateLimiter` — 0 tests (cooldown/window/LRU
  eviction unexercised); `Crypto` — 12-line smoke only (HMAC, constant-time compare, lazy
  rehash, secure-token, malformed-hash timing all untested). *(med-high)*
- **Core untested modules**: `LruCache` (0 direct), `ProtocolLoader` (0 — JSON load /
  IdOrThrow / id parsing), `Logger` (0 — the JSON-escaping hole is unobserved). *(med)*
- **Hot-reload paths untested**: `PluginHost::Poll()` / debounced watcher (0 coverage);
  the fresh-reload `Reload(false)`/`ResetRegistry` branch; the rollback-Init-failure edge
  (bug #6). *(med)*
- **Serialization**: no malformed-input test for scene JSON (bug #7 unobserved); resource-
  loss (#2) is unasserted. **NEON**: 0 test references — the whole 4-wide path is unexercised.

---

## D. Roadmap-level gaps (strategic; largely by-design deferrals)

Milestone ground truth: M0–M6 done (Core, renderer core, input, sim substrate, plugin host,
physics port). **M7 = Grimoire editor shell + sim-time control → LevelEditor is next and
not started** (no `Grimoire/` project exists). Ordered critical path to a playable
engine-native vertical slice:

1. **Decide to close out the physics arc** — a conscious decision point, not a blocker.
   Further MT/SIMD/units polish is diminishing returns against zero consumers.
2. **M7 Grimoire shell + sim-time control** *(XL)* — the next planned milestone; also
   supplies the RunLoop pause/step/time-scale that's currently missing (Section B).
3. **Render-graph → bindless → batcher-v2 → materials** *(XL)* — the renderer spec's own
   dependency chain, stalled since M2b; prerequisite for lighting/particles/tilemaps.
4. **Engine-side UI runtime module** *(XL)* — the single largest content-blocking gap.
   No `UI/` dir in `Arcane/Arcane/src/Arcane`; the BehaviorGraph is authored but has **no
   execution engine anywhere in Arcane**. 32.7K LOC of Lua UI to port, no oracle.
5. **Game.dll stood up** *(XL)* + **client-side netcode** *(M)* — Core has `Net/` (server-
   flavor) but `Arcane.dll` never links it; there's zero client session/protocol/reconnect
   integration. This turns the engine from "demo host" into "the client can run inside it."
6. **Combat framework** *(L)* + **tilemap/overworld renderer** *(M-L)* — built fresh against
   the now-real engine (deferred "Combat Sphere" per every spec).
7. **Animation** *(L)* + **particles/VFX** *(L)* — fill visual fidelity once the render-graph
   substrate (step 3) exists to host them.
8. **Asset import/build pipeline** *(M-L, explicitly deferred)* + **packaging/shipping**
   *(M, not on any roadmap)* — last-mile, correctly deferred but presently zero-progress.

Also under-specified (not on any milestone list): **audio mixing depth** (device-init only),
**general profiler UI** (Tracy + debug-viz are physics-scoped only).

---

## E. Physics known-issues triage (corrected against live code)

Two memory-flagged items are **stale/closed**:
- *Collision filter "never enforced"* — **now implemented + tested** (`PhysicsCollisionFilterTest.cpp`
  asserts disjoint category/mask never contact, compatible do, defaults collide). Not a gap.
- *Unwalled-sim escape crash* — mitigated: `SpatialGrid.cpp:41` now has an `isfinite` AABB
  guard + the clamp/SaneBox magnitude bounds landed. Worth a stress re-confirm under
  finite-but-huge coords, but not the open crash the note implied.

Still open (parity/robustness, mostly deferred-by-design and orthogonal to gameplay):
- Kinematic/pinned bodies not speed-clamped vs Box2D (awake-set integrates dynamics only).
- Gravity/damping ordering divergence vs Box2D (Arcane damps gravity; Box2D adds undamped
  delta after damping).
- Island split-linkage GAP A (joints) + GAP B (sensors) — from the MT parity program.
- `EmitContactConstraints` no-sleeping-dynamic invariant tripped by dynamics on tile-spans.
- Restitution-rebound test tautology (`apexY`/`reboundHeight` vacuous, y-down convention) —
  just found in MKS P2; pre-existing, proves nothing about real rebound.
- Deferred-by-design (tracked in the MKS plan): CharacterController kMaxSubstep/kDepenetrationSkin
  (P4), kShapeCastTol re-couple (P4), MouseJoint maxForce (P5) — the `MKS-DEFER(Pn)` markers.
- SpatialGrid Cycle 2 (opt-in first-class mover-broadphase) and `m_staticList` consolidation —
  deferred/data-gated feature decisions.

---

## Suggested next steps (menu, not a plan)

- **Quick wins**: bugs #1, #3, #4, #6, #7 are each S–M and self-contained; a "engine
  robustness" mini-batch would close the confirmed latent crashes/leaks cheaply.
- **One theme worth a real pass**: the persistence/serialization spine (#2 + Section B
  serialization items) — it's the weakest real area and it's the M7/Grimoire predecessor.
- **Security/test debt**: RateLimiter + Crypto coverage before any Linux port work.
- **Strategic**: the roadmap section is the honest "the game doesn't exist yet" picture —
  worth a deliberate decision on when to stop polishing physics and start M7/UI-runtime.
