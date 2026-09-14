# Engine ceiling, Deadlock-class 3D, and Box3D — binding north star

**Date:** 2026-09-14
**Status:** binding user rulings (Grok session, then written for Claude)
**Read before:** any 3D renderer spec, any 3D physics spec, any “is this AAA yet” argument, any Manifold3D / Jolt / PhysX fork
**Does not replace:** `docs/research/2026-08-12-deadlock-render-target.md` (T1–T7 feature contract) or `docs/research/2026-08-21-3d-foundations-assessment.md` (F1 pivot + Box3D vendor choice). This document is the *ceiling, sequencing, and physics-lineage* layer on top of those.

Related:
- Renderer contract: `docs/research/2026-08-12-deadlock-render-target.md`
- **Public origins of the Source 2 renderer (legal cites only):** `docs/research/2026-09-14-source2-renderer-public-origins.md`
- 3D pivot + “vendor Box3D after F1”: `docs/research/2026-08-21-3d-foundations-assessment.md` (F1 has landed; Box3D is now unblocked)
- 2D physics ownership: `docs/specs/2026-09-11-physics-2d-wiring-design.md`
- Box3D announcement (primary source for lineage): https://box2d.org/posts/2026/06/announcing-box3d/
- Box3D repo: https://github.com/erincatto/box3d

Reference-code policy unchanged: **no leaked Source 2 source, ever.** Legal channels remain VRF (MIT), Valve published talks/wiki, Epic GitHub read-only, and MIT/Apache papers. Dirk Gregorius’s private Rubikon-Lite tree is **not** a legal vendor target — Box3D is the public descendant.

---

## 1. What this engine is allowed to become

Arcane is a 1-human + Claude engine, in C++ since ~May 2026 (Love2D-era March if counted; this repo’s history is 2026-05-17 → present, ~2000 commits).

| Ceiling | Grade | Meaning |
|---|---|---|
| Craft of a given system | ~8.5–9 / 10 | Already near this. More years will not 2× golden-gate honesty or plugin ABI discipline. |
| Product, 2D / 2.5D | ~8 / 10 | Unity-2D / Godot-2D class. High-probability ceiling if UI, 2D animation, particles, and audio events actually get consumed by Aphelyon. |
| Product, 3D AA (Deadlock *picture*) | ~7 / 10 | Clustered deferred or forward+, GGX, baked GI, CSM, cubemaps, NPR-on-PBR, TAA. Not Nanite, not Lumen. |
| Product, AAA (UE5 replacement) | ~3 forever at this headcount | Organizational, not talent. Do not plan as if Nanite/Chaos/World Partition will appear. |

**User-stated 3D visual target: Deadlock / Source 2 the renderer, not Unreal.** Nanite is an explicit non-goal. Source 2 *the platform* (Hammer, ModelDoc, Panorama, Rubikon-as-Valve-internal, Source netcode) is also a non-goal as a product clone.

A 2026-09-14 live-source audit (not the May 2026 Love2D AAA paper, not the July 2026 gap survey) scored the tree at **~5.5 / 10 overall**: **~3.2 feature coverage vs shipping AAA**, **~7.6 craft of what exists**. That split is the point. Do not “fix” the 5.5 by opening eight new spines.

---

## 2. Deadlock-class means the *picture*, not Valve’s org

`2026-08-12-deadlock-render-target.md` remains the feature contract (T1–T7). Interpret it as follows.

**Attainable (the 3D north star):**
- T1 light grid + GGX + mesh materials that actually stitch + an explicit forward-vs-deferred decision
- T5 post (histogram exposure, split bloom, GTAO, TAA) + T6 citadel NPR (`citadel.slang` is MIT in VRF)
- T3 *lite*: stable CSM + box-projected cubemap IBL
- T4 *lite*: gradient fog + cubemap MIP fog (high look-per-week, distinctly Source 2)
- T2 *lite*: a baker you will actually run on maps you author (one lightmap + probes + one cubemap per volume). Grow toward SH2 / stationary masks only if the game is indoor and baked.

**Trap (do not start because UE did):**
- T3 scene distance field feeding DF shadows/AO/reflections/outlines. UE spent years. JFA pick-outlines already exist; keep them until a game scene needs DF shadows.
- Full VRAD3 parity as the first baker.
- T1–T7 as one simultaneous arc.

**Not a renderer problem (separate products):**
- Skeletal animation (glTF import currently skips skins / morphs / `JOINTS_0` / `WEIGHTS_0`)
- 3D physics feel (Box3D + movement — §4)
- Game HUD (MSDF `Glyph()` still binds a white texel; game UI is ImGui in `OnDrawUI`)
- Replication / Panorama / particles / packaging

`mesh.hlsl` is Lambert on purpose (“DELIBERATELY NOT PBR”). T1 is the cutover to GGX, not a second half-PBR model beside it.

---

## 3. Time estimates — use *this repo’s* unit, not department-years

Measured cadence (git, not memory):

| Arc | Calendar |
|---|---|
| NRI wholesale swap + `RenderGraph` | spec 2026-08-12 → declare/compile 08-13 → 3D slice blocked on *scene* 08-21. **~9 days** |
| F1 2D→3D transform + F2a `MeshRenderer` | 2026-08-21–22. **~1 day** |
| F2b cook + BC7 + bindless | spec **and** `CookSession` + CLI + CI `--check` + editor watcher **2026-09-04** |
| F2c mesh import | importer + bake 2026-09-10, closeout ~09-11. **~2 days** |
| July physics deepening | ~3–4 weeks of SIMD/MT/islands. Library ~7.5, game wiring was ~4.5 until the Sept 11 2D-wiring arc |

**Demonstrated unit:** a well-specced architectural arc is **1–3 weeks**, often less.

What compresses: new subsystems with a frozen spec, ports of known papers / VRF math, dual-API, cook, ABI, golden gates.

What does **not** compress: TAA smear, cascade shimmer, a bake an artist would keep, animation feel, “does this screenshot read as Deadlock?” Those fail the eye, not Catch2.

### Recalibrated focused renderer time (1 human + Claude, current process, one stream)

| Milestone | Focused time | Wall clock if also shipping Aphelyon (~×2–3) |
|---|---|---|
| T1 (grid + GGX + mesh materials + forward/deferred) | **2–5 weeks** | — |
| T5 + T6 (exposure, bloom, NPR, first TAA) | **3–6 weeks** | — |
| T3 lite (CSM + cubemap IBL) | **2–4 weeks** working, **+2–4 weeks** to not shimmer | — |
| Greybox Deadlock look (T1+T5+T6+CSM, no baker, no SDF) | **~2–4 months focused** | **~8–12 months** |
| T2 lite (a room you’d keep) | **~6–10 weeks** to a room, **~3–4 months** usable indoor | — |
| T2/T3 *full* contract (SH2 pages, stationary masks, scene SDF) | **~6–12 months focused** if it is *the* job | SDF still the trap |
| Source 2 the platform | not a renderer estimate | do not schedule |

A Deadlock-like untextured hero in a boxed interior (NPR on, sun shadow, TAA on) as a **focused 3D block** is months, not years — *if* T1 is specced like NRI was, and July-physics diversions are refused.

Do **not** start T1 while the 2D game still cannot draw text. The 3D north star survives a year of shipping Aphelyon UI/animation; it does not survive opening T2 and T3 before T1 has a consumer.

---

## 4. Physics — Box3D indefinitely; Manifold3D later

### Ruling

**Vendor and run [Box3D](https://github.com/erincatto/box3d) (Erin Catto, MIT, C17, C API) as the 3D physics engine indefinitely**, until a 3D game has actually consumed it and a Manifold3D arc is justified. F1 has landed, so the 2026-08-21 “not until F1” gate is open.

**Manifold3D**, when it exists, treats **Box3D as the citation/oracle** the way Manifold2D treats Box2D. It is *not* a download of private Rubikon-Lite.

### Lineage (Catto, 2026-06-30, primary source)

1. Dirk Gregorius suggested forking hobby **Rubikon-Lite** (Alyx / Source 2 physics).
2. Catto hooked that into Unreal.
3. He then replaced almost all APIs, data structures, and algorithms with **Box2D** code.
4. What remains of Rubikon-Lite in Box3D is **convex hull generation and some collision**. The rest is Box2D architecture plus new 3D work.

So “Manifold3D based on Rubikon-lite and Box3D” is **one lineage**. Box3D *is* that mix. Do not seek Dirk’s private tree.

Box3D is also in s&box (Facepunch), Esoterica, and Glenn Fiedler’s space game. Source-adjacent games already run it.

Chosen over Jolt (already recorded 2026-08-21): Rubikon heritage, Box2D-fork so Manifold3D has a continuous citation path, C17/C API matches vendoring. **Accepted risk:** Box3D is still young (v0.1-class, no API stability). Pin a commit. Wrap a thin C++ façade. Do not spray `b3*` types through `PhysicsSystem`.

### Two worlds, engine-owned (same R1 as 2D wiring)

| | 2D | 3D |
|---|---|---|
| Solver | Manifold2D | Box3D (later Manifold3D) |
| Components | `RigidBody2D` / `Collider2D` / `PhysicsBodyRef` | new `RigidBody3D` / `Collider3D` / a 3D body-ref |
| Write-back | XY + Z-turn; **destroys** out-of-plane rotation on purpose | full quat |
| Gravity / units | +Y down, MKS | decide at Box3D-wiring spec time; MKS stays |

`Runtime` owns both worlds. Game modules never include Manifold2D or Box3D headers (2D wiring R1). Do **not** teach today’s `PhysicsSystem` to be 3D — it will flatten 3D poses.

### Source *feel* is not the solver

Box3D gives Soft Step, CCD, capsules/hulls/trimeshes/heightfields, a character *mover*, queries. Classic Source movement (traces, ground, duck, step-up, wish-dir, predicted projectiles) is a **game system on top of those primitives**, same as it was on VPhysics/Rubikon. Do not expect HL2 air-accelerate from `b3World_Step`.

Collision for 3D is cooked shapes, not graphics meshes as colliders.

### When Manifold3D is allowed

Only after a 3D mode is playable on Box3D and the list of missing Rubikon-isms is *measured* (hull cooking, compound bake, ghost collision). Manifold3D is a later year, not the vendor arc. July 2026 2D physics is the prohibition: do not spend a month making a 7.5 library while the consumer is a 4.5.

Vendor + ECS wiring + debug draw is **F2a-shaped** (days to a couple of weeks). Character controller + a stairs/ramp/crate test map is the feel work.

---

## 5. Sequencing (do this order)

**2D / Aphelyon (the only shipping consumer today):**
1. Font atlas + retained game UI (`Glyph()` is a white texel today)
2. 2D animation, then particles
3. Audio events the game actually `Play()`s
4. Client protocol to Auth/Account/Combat (Core TCP already exists; that is services, not replication)

**3D renderer, when 3D is the job:**
1. T1
2. T5 + T6
3. T3 lite (CSM + cubemaps). Skip SDF.
4. T4 lite
5. T2 lite

**3D physics, when 3D is the job:**
1. Vendor Box3D behind a façade, parallel to Manifold2D
2. `RigidBody3D` / colliders / write-back / overlay
3. Character mover + traces + a movement test map
4. Manifold3D — later, oracle = Box3D

Keep: dual API, frame graph, linear HDR, last-good material compiles, plugin ABI / `arcbuild` / golden gate, cook as a product, explicit non-goals.

Do not spend a year on: Manifold2D as a Box2D peer no 3D scene uses; GPU-driven / mesh shaders / RT because caps bits exist; a scripting VM (the C++ module *is* the scripting story); packaging platforms that will not be certified; scene SDF; private Rubikon source.

---

## 6. Rulings ledger (2026-09-14)

| # | Ruling | Rejected |
|---|---|---|
| C1 | 3D visual target = Deadlock / Source 2 **renderer**. Nanite / Lumen / UE5 replacement are non-goals. | “AAA” as Unreal-competitor |
| C2 | Source 2 **the platform** is not the clone target. | Hammer / Panorama / Source netcode parity |
| C3 | T1–T6 (with T2/T3 *lite*) is the 3D picture. Scene SDF and VRAD3-parity trail, they do not lead. | T1–T7 as one arc |
| C4 | Time in **this repo’s** units: specced architecture = weeks. Look-dev = extra weeks of eye. Department-years are forbidden as estimates here. | 18–24 month T1 |
| C5 | Do not start T1 while 2D text/UI is still a hole, unless 3D is explicitly the only job that quarter. | Renderer-first while Aphelyon cannot draw glyphs |
| P1 | **Box3D indefinitely** as 3D physics. Pin commit, C++ façade, engine-owned world. | Jolt as default; PhysX; teaching `PhysicsSystem` 3D |
| P2 | Manifold3D later; **oracle = Box3D**. Box3D already *is* Rubikon-Lite + Box2D. | Vendoring private Rubikon-Lite |
| P3 | Two physics worlds. 2D stays Manifold2D. | One solver for both |
| P4 | Source feel = movement/traces on top of Box3D, not a solver flag. | “Box3D ⇒ HL2 movement” |
| P5 | No Manifold3D until a 3D game consumes Box3D. | Immediate 3D physics rewrite |

---

## 7. Snapshot of the tree this document was written against

Live-source, 2026-09-14, not recalled:

- Dual-API NRI, barrier-correct frame graph, RGBA16F, ACES, 2D batcher, opaque Lambert mesh, bindless albedo 256, runtime DXC, sprite+fullscreen material graph (mesh `.arcmat` is albedo+tint only)
- Editor: outliner, inspector, GPU pick, 2D gizmos, play snapshot, shader graph, cook queue. Preferences/Project Settings menu items are no-ops. Gizmo is 2D by design (F4).
- Cook: PNG + glTF/GLB only. Skins/morphs/anims refused. Audio/font/scene not cooked.
- ~1709 CPU test cases / ~57k assertions; 34 GPU pixel tests; 4-lane golden gate with self-test; Tracy vendored and **unlinked**
- `ArcaneServer` is `return 0`. No gameplay replication.
- Audio mixer exists; spatialization compiled out; reference game never `Play()`s.

July 2026 gap survey and May 2026 Love2D AAA audit are **stale**. Do not resume work from them.
