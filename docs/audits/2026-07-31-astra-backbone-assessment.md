# Astra as Arcane's backbone — honest assessment

**Date:** 2026-07-31
**Question asked:** what state is Astra actually in, and what workarounds, pain
points or jank has using it as Arcane's backbone produced?

---

## Verdict

**Astra is in materially better shape than Arcane's own comments about it claim,
and the integration is unusually clean for a dependency this deep.** Across ~80
Arcane files that touch Astra, there are **six** explicit complaint-comments.
That is a remarkably low friction rate for a library that owns the registry, the
reflection that drives scene serialization, the state that drives undo/redo, and
the data model the entire Inspector is generated from.

The pain that does exist is **not diffuse**. It concentrates in one seam:
**the boundary Astra cannot test from inside itself** — what happens to
Astra-owned state when a *host* loads and unloads modules. Everything expensive
this project has paid to Astra sits there.

Two Arcane comments are now **stale in a way that understates Astra** and should
be corrected (details below); one of them is actively telling readers a working
capability is unsupported.

---

## 1. What the vendored Astra actually contains

Arcane vendors `include/` only, synced 2026-07-29 from dev `f8c9998`. Re-measured
2026-07-31: **67 of 68 headers byte-identical** to the dev repo (the 68th is this
session's own fix). The vendored copy is current, not drifting.

Astra ran its own correctness program and **it landed**:

- `docs/reviews/2026-07-25-astra-tier0-correctness-register.md` — consolidates a
  9-agent review plus a **147-agent blind full review** ("assume-nothing"; every
  finding adversarially verified at ≥2/3).
- Result: **4 Criticals (CR-1..CR-4) + 25 Importants (IM-1..IM-25) + 3 LZ4 codec
  fixes**, batch-fixed, then a 5-reviewer pre-merge pass that caught 2 more
  Importants and 2 Minors.
- **Both commits (`66277af`, `15a71f8`) are contained in `dev`** — verified with
  `git branch --contains`. The register's own header still says the branch is
  "NOT merged"; that note is stale.

Spot-verified **in Arcane's vendored tree**, not taken on trust:

| Finding | Nature | Verified present |
|---|---|---|
| CR-2 command-buffer null-page write | memory-unsafe (Release) | `CommandBuffer.hpp` — 20 × `m_recordFailed`/`AllocationFailed` |
| CR-3 `MetaRegistry` hash-collision aliasing | memory-unsafe (wrong-size construct) | `MetaRegistry.hpp:63-122` — identity compared, collision refused loudly |
| CR-4 archetype mask id-desync | silent data-integrity | `Archetype.hpp:1073-1145` — `localMask` rebuilt from hash-resolved descriptors |

**So Arcane is not shipping on a known-unsafe ECS.** That was the single most
important thing to establish, and it checks out.

## 2. Two stale comments that now misinform

**`Base/Runtime.cpp:155-162` is wrong and consequential.** It states that "the
vendored Astra still rebuilds an archetype straight from the on-disk mask words
(`Archetype.hpp:871`)", that the fix is "already fixed upstream in the dev Astra
(marked CR-4) and lands on the next vendor sync", and concludes: *"Until then,
treat cross-process Save/Load of a registry snapshot as unsupported."*

That sync **already happened** (2026-07-29). CR-4 is in the vendored copy. The
cited line number no longer even points at the code it describes. A reader today
would avoid a capability that works. **Fix the comment; re-evaluate whether
path-based cross-process registry Save/Load can be supported.**

**The register's "NOT merged" banner** is likewise stale (see §1). Worth a
one-line correction upstream so the next reader doesn't repeat this check.

## 3. The real gap — and why Astra's own review missed it

The defect that cost this project the most (an intermittent editor crash/hang on
project switch, two sessions of failed symptom-reasoning) was:

> `ComponentDescriptor` holds raw function pointers into whichever **module**
> registered the type. Nothing removed them when that module unloaded, and
> `RegisterComponent` is **first-wins**, so a reloaded module could never rebuild
> them — the dangle was *permanent*, not transient.

**This appears nowhere in the Tier-0 register.** 147 blind agents over the whole
codebase did not find it. That is not a criticism of the review — it is the
finding: **the defect is invisible from inside Astra.** It requires a host that
`FreeLibrary`s a module which had registered types. Astra's tests, by
construction, never unload anything.

Corollary, and the thesis of this assessment: **Astra's weak spot is not
correctness-in-the-small — it is the host-integration seam it cannot exercise.**
Every expensive item below lives there:

1. No module-lifetime story (fixed 2026-07-31 by *adding* `UnregisterModuleRange`
   — the API did not exist).
2. First-wins registration turning a transient dangle permanent.
3. `ReRegisterComponent` as a **hand-maintained** workaround: every plugin lists
   every engine type it touches (`Sandbox.cpp:113-122` = 10 calls,
   `PlaygroundGame.cpp:118-123` = 6). Miss one and it silently misbehaves.
4. `Runtime.cpp:124-131` records the consequence in its own words: before the
   engine registered its own roster, the live roster was "whatever the hosted
   game plugin happened to `ReRegisterComponent<T>()`", which left the editor's
   Add Component catalog showing only the plugin's types and **silently dropped
   `Identity`/`Hidden`** when a runtime host loaded an editor-saved scene.
5. ComponentIDs are a per-process monotonic counter, **not** hash-stable — and a
   prior comment claiming otherwise was flatly false (`Runtime.cpp:140-149`).
6. TypeContext must be injected by hand across modules; forgetting it is an
   established local bug class.
7. **`MetaRegistry` still has only an all-or-nothing `Clear()`** — the same
   module-lifetime gap therefore remains open for `TypeMeta`. Engine types are
   safe only because Arcane.dll registers them first. **KNOWN, UNFIXED.**
8. **`PluginABI.hpp:28` — ABI v5 was forced by an Astra re-vendor** ("the
   re-vendored Astra migrated its threading seam"). Astra's release cadence is
   coupled to Arcane's plugin ABI.

## 4. Attempts to falsify the thesis

Ran deliberately, because a thesis that only gets confirmed is worthless.

- **IM-24 adaptation lag.** The batch made `AddComponent`/`AddComponentByID`
  return `bool`; Arcane checks it at **one of nine** call sites. Looked like a
  second, independent theme (API-evolution drift). **Refuted on inspection:**
  `false` means *only* a stale/invalid entity handle, Astra deliberately left the
  return non-`[[nodiscard]]` "so existing statement-style callers are
  unaffected", and every Arcane call site passes a freshly-created entity. Not a
  defect.
- **Diffuse-friction hypothesis.** Swept every `Astra`-adjacent complaint comment
  in Arcane's own source (`.example/` excluded — the vendored UE tree floods this
  query). Only six, and they are small: no container-element recursion
  (`PhysicsComponents.hpp:221`), a non-existent `Astra::TypeHash<T>()`
  (`InspectorFields.cpp:3`), `ReadOnly` shows-but-cannot-edit
  (`InspectorView.cpp:399`), `Astra::Entity` is a using-alias so it cannot be
  forward-declared (`Scenes.hpp:35`), a Fixture descriptor that cannot express a
  body (`Scenes.cpp:30`), and a descriptor-by-hash shape that does not exist
  (`CommandStackTest.cpp:583`). **Thesis survives.**

## 5. Arcane-side layers built on Astra — keep or upstream?

Not defects; open design questions worth a decision rather than drift:

`Serialization/RegistrySnapshot` (whole-registry snapshot/restore for play-mode +
hot-reload), `Serialization/ReflectionJson` (Astra ships **binary**
serialization; JSON is Arcane's own), `Serialization/SceneSerializer`,
`Edit/ComponentEditCommand` + `Edit/RegistryStateCommand` + `Edit/EntityOps`,
`Scene/SceneModule` + `Scene/PhysicsComponents` roster helpers,
`Sim/SystemSchedulers` + `Sim/Simulation`, and the editor's `ComponentCatalog` /
`InspectorFields` / `InspectorMeta`.

My read: the **editor-facing** ones are correctly Arcane's (they encode editor
policy). `RegistrySnapshot` is the interesting one — it is generic, it is exactly
the host-integration seam Astra is blind to, and pushing it upstream would give
Astra a reason to grow tests for module reload.

## 6. Recommendations, in priority order

1. **Fix `Runtime.cpp:155-162`** and re-evaluate cross-process registry
   Save/Load. It is currently documented as unsupported for a reason that no
   longer exists.
2. **Close the `MetaRegistry` half of module lifetime** (§3 item 7) — the
   `ComponentRegistry` half shipped today; the reflection half is still open.
3. **Retire `ReRegisterComponent` for ENGINE-owned types — it is now obsolete,
   and it was always the thing manufacturing the fragility.** `Sandbox.cpp:109-111`
   states its own rationale: re-register "so its function pointers target the
   plugin's code, **not the previously loaded image's**." That is a workaround for
   a missing unload purge — which now exists. What it costs: it takes descriptors
   that safely pointed at the never-unloaded `Arcane.dll` and re-points them at a
   module that unloads on every reload, i.e. it *creates* the dangle this arc
   fixed. It is also safe to stop: the workspace already mandates identical
   headers under identical flags across modules (see the `links{}` note on the
   Sandbox project), so `Arcane.dll`'s instantiation is ABI-correct to call from a
   plugin. **New rule: a plugin registers only the types it OWNS.**
4. **Make module ownership first-class, replacing address-range inference.**
   `UnregisterModuleRange` works by testing whether a descriptor's function
   pointers fall inside an image — reconstructing at unload time something that
   was known at registration time. Better, and platform-free:
   ```cpp
   { Astra::ModuleScope scope(creg, moduleId);   // thread-local "current owner"
     creg->RegisterComponent<Foo>(); }           // auto-tagged, signature UNCHANGED
   creg->PurgeModule(moduleId);                  // on unload
   ```
   An RAII scope rather than an extra parameter is the point: a per-call owner
   argument would recreate the hand-maintained-list problem it is meant to remove.
   Keep the range overload as the fallback for registrations Astra never saw.
4. **Give Astra a host-lifetime test lane** — load/unload a module that registers
   types. It is the one class of defect its otherwise-excellent review process is
   structurally unable to reach.
5. Upstream the "NOT merged" correction to the Tier-0 register.

## 7. Not covered

- No performance or memory benchmarking. Astra has `bench-compare/` with EnTT and
  flecs harnesses; none of it was run or read.
- Astra's own 814-test suite passing says nothing about Arcane's integration —
  the integration is covered by ArcaneTests (31409/710), which until today had
  **zero** coverage of plugin-unload descriptor lifetime.
- `Registry`, `View`, `Query`, `ArchetypeManager` internals were not reviewed;
  this leaned on Astra's 147-agent review for intra-library correctness rather
  than repeating it.
- The Linux/macOS story (IM-16 was a macOS build break) was not examined.
