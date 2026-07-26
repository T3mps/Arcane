# Review: Outliner slice 4 + Inspector multi-select + Hub slice 1

Three independent adversarial reviews of `ed597536~1..07e1333f`, run 2026-07-26.
All of that range was built INLINE with self-review only — this is the first
independent look. Lenses: correctness/ImGui-composition, engine blast radius,
test adequacy.

**Verdict: 2 of 3 reviewers say NOT ready. Fix before building on it.**

---

## DISPOSITION (2026-07-26, same day)

Every finding worked. Gate green at **29724 assertions / 530 cases**, verified
under `--rng-seed 6`, `--rng-seed 17`, and two random seeds (2010332980,
1809940436). Build clean; the only warnings are the pre-existing C4251
dll-interface noise on `std::` members.

| # | Finding | Disposition |
|---|---|---|
| CRITICAL 1 | `ApplyImmediate` commits a transaction it does not own | **FIXED.** `CommandStack::Begin` now mints a `TransactionId` owner token; `Commit`/`Cancel` ignore any other token, so a joiner and a stale owner are both inert. Gizmo parks its token in `GizmoDrag::txn`; the Inspector's cross-frame gesture parks it in the new `InspectorState`; both single-shot immediate paths use `ScopedTransaction`, which now only closes what it opened. 5 new regression cases. |
| MAJOR 2 | Two `material` fields collide on one popup id | **FIXED.** `PushID(ci.descriptor->hash)` scopes each component section (branches renested so the ID stack stays balanced — no `continue` between push and pop). Asset-ref button id switched to `###assetref`; verified against `ImHashStr` (`imgui.cpp:2557`) that only `###` resets the hash. |
| MAJOR 3 | `EntityInfo` add/remove wipes the durable Guid | **FIXED** via the hide-list, as the audit's first option. Decision recorded in `ComponentCatalog.hpp`: the Outliner owns this component through create + rename, and a descriptor-driven add cannot mint a per-entity Guid without a post-construct hook nothing else needs yet. The roster-registration test no longer witnesses `EntityInfo` through the catalog, so it now asserts registration against the `ComponentRegistry` directly. |
| MAJOR 4 | The gate strands `File → Open Project` | **FIXED**, second option. A bare *interactive* launch boots and raises the shipped picker on frame 1 (`EditorApp::RaiseOpenProjectOnStart`, routed through `MenuRequests` so there is one launch site). A *scripted* run (`--frames N`, no project) still exits 2 — verified by hand. Note: `ArcaneEditor` MUST stay a ConsoleApp; the console flash the audit noted is the price of the probe's stdout, and a WindowedApp would silently break the Hub. |
| MAJOR 5 | `EngineInfoJson` takes `argv[0]` | **FIXED.** New `ARCANE_API Arcane::ExecutablePathUtf8()` (GetModuleFileNameW + explicit `CP_UTF8`, grow-and-retry, forward-slashed). `EngineInfoJson` now takes UTF-8 *bytes* rather than a `std::filesystem::path`, so nothing re-encodes through the native narrow encoding, and `dump()` runs with `error_handler_t::replace` so a malformed byte can never throw out of `main`. Verified by hand on both hosts: exactly one stdout line, absolute path. |
| — | Runtime roster comment factually wrong | **FIXED.** Re-verified in the vendored source before rewriting: `TypeContext.hpp:93` is `m_next++`, and vendored `Archetype.hpp:871` still rebuilds from the raw on-disk mask. The comment now states the real invariant (ids are process-local and counter-assigned; only the hash is stable) and names the vendor-sync dependency. |
| — | ABI tripwire is a tautology | **FIXED, and made real.** Comment corrected in `HostBootTest.cpp` and in the plan's Verification Summary. Beyond the comment, minor 7's `PluginABIVersion()` means the probe now reports the **DLL's** ABI while the test compares against the **test exe's** constant — so a stale-binary mismatch is now genuinely detectable, which is the failure the design cared about. |
| Minor 1 | Structural edits refuse silently | **FIXED.** One `CanEditStructure(undo, binding)` predicate (`editMode && !InTransaction()`) now disables every structural affordance: both context menus, Add/Remove Component, and the drag-reparent source/target. |
| Minor 2 | Add-Component popup rows not gated on editMode | **FIXED** — same predicate, evaluated once per popup. |
| Minor 3 | Int fields lossy through `float`/`%.3f` | **FIXED.** `MultiScalarRow` carries `double` + an `integral` flag; ints format `%lld`, parse via `strtod`, and clamp before the narrowing cast (out-of-range double → int32 is UB). |
| Minor 4 | AssetRef mixed computed then discarded | **FIXED.** Mixed asset refs render `--`, and the clear button is offered on a mixed selection. |
| Minor 5 | `kAddComponentPopup` comment inverted | **FIXED.** Verified `OpenPopup` seeds the id with `g.CurrentWindow->GetID` (`imgui.cpp:12956`): the two sites' ids DIFFER, and the shared buffer is justified by one-popup-at-a-time. |
| Minor 6 | Probe shares stdout with the log sink | **FIXED.** Both the `Arcane` logger and Core's console sink moved to **stderr**. The editor Console reads the Mosaic sink, so it is unaffected. |
| Minor 7 | Probe reports the exe's ABI, not the DLL's | **FIXED.** New `ARCANE_API uint32_t Arcane::PluginABIVersion()` compiled into Arcane.dll; the probe calls it. |
| Minor 8 | `Runtime`'s `externalContext = nullptr` default | **FIXED.** Default removed — every caller already passed a context, so the TypeContext-theft guardrail is now compiler-enforced instead of conventional. |
| Minor 9 | `m_componentNames` never trimmed | **DEFERRED, deliberately.** It lives in vendored `ThirdParty/Astra/.../ComponentRegistry.hpp:178` where it is already documented as an accepted upstream tradeoff (pointer-stable `c_str()` storage). Patching vendored source would be lost on the next Astra sync — same lesson as the Manifold2D vendor-back. Belongs upstream, alongside CR-4. |
| Tests | 4 vacuous/weak + 1 latent | **FIXED.** `ClassifyField` gained a per-arm witness test (Bool/Float via `RigidBody2D`, Vec2/Float via `Transform`, and a local reflected `ClassifyProbe` for Vec3 — nothing in the engine roster declares a `glm::vec3`, which is exactly why that arm was unpinned). Sticky mask gained the `1,2,2` fixture. The forward-slash assertion is now fed backslashes. `MixedWorld::TransformHash()` REQUIREs a non-zero hash instead of returning 0. |

**A bug introduced by the CRITICAL fix, caught and closed during the work:**
clicking field B while field A is mid-gesture moves ImGui's ActiveId in one
frame, and if B is drawn ABOVE A then B activates before A's `EndGesture` runs.
B's `Begin` would see A's transaction still open, get `None`, and A's later
`Commit(None)` would no-op — stranding A's transaction open forever on stale
`before` bytes. `BeginGestureIfActivated` now commits a still-parked token before
opening its own.

**STILL OWED: desk-verify.** None of the ImGui surfaces have automated coverage.
Specifically worth clicking: the CRITICAL repro (type into a multi-select
`position.x`, do not press Enter, then drag a gizmo handle — the drag must be one
undo step), two `material` fields on one entity (`SpriteRenderer` +
`PostProcess`), a bare `ArcaneEditor.exe` launch raising the picker, and the
disabled-during-gesture structural menus.

---

## CRITICAL 1 — `ApplyImmediate` commits a CommandStack transaction it does not own

`EditorPanels.cpp` `ApplyImmediate` / `ApplyGuidImmediate`; `CommandStack.cpp:14-21`.

`CommandStack::Begin` is documented "no-op if one is open (keep the first)". So a
caller must know whether it actually opened the transaction before calling
`Commit()`. `ApplyImmediate` does not — it calls `Begin` then unconditionally
`Commit`.

Repro (traced against `EditorApp::MainLoop` frame order):
1. Multi-select two entities with `Transform`; `position` renders as a
   `MultiScalarRow`.
2. Type `5` into `position.x`, do NOT press Enter.
3. Press LMB on a gizmo handle in the Viewport.

In that one frame: the gizmo block (`EditorApp.cpp:959`, runs off the raw SDL
snapshot BEFORE ImGui BeginFrame) calls `m_undo->Begin("Gizmo")` + snapshots.
Then `DrawInspectorPanel` runs; `InputTextEx` clears `ActiveId` on ANY LMB click
anywhere (`imgui_widgets.cpp:4963-4965`), so `IsItemDeactivatedAfterEdit()` fires,
`ApplyImmediate` runs, its `Begin` no-ops, and its **`Commit()` closes the
gizmo's transaction**. The rest of the drag mutates Transforms against a closed
stack; mouse-up `Commit()` no-ops.

**The entire gizmo drag becomes un-undoable, silently.**

Same root cause, two more shapes: `EndGesture()` can `Cancel()` a foreign gesture
(`EndGroup` forwards the Deactivated flag, `imgui.cpp:12525-12530`); and the
gizmo's own `Cancel()` at `EditorApp.cpp:1045` can discard someone else's.

This is an architectural seam, not a typo: three independent input consumers
(gizmo drag, Inspector field gestures, `ApplyImmediate`) all call `Commit`/
`Cancel` unconditionally on a shared no-op-if-open `Begin`.

**Fix:** make ownership explicit — `Begin` returns bool ("you opened it"), and
only the opener may `Commit`/`Cancel`. Alternatively a scoped/token API where a
nested `Begin` is a hard error. Note `ApplyGuidImmediate` predates slice 4 and
has the same shape, so this bug is older than this range; `ApplyImmediate`
widened it.

## MAJOR 2 — the Inspector pushes no per-component ID scope; two `material` fields collide

`EditorPanels.cpp` `InspectEntity` loop + `PushID(f.nameHash)`.

`FieldInfo::nameHash` hashes the BARE field name. The loop never pushes a
per-component id, so `Arcane::SpriteRenderer::material` and
`Arcane::PostProcess::material` (`Components.hpp:83, 95`) get the SAME id seed.
Both `BeginPopup("##assetpick")` calls then resolve to one popup id and both
return true in the same frame, appending the asset list twice with duplicated
`Selectable` ids; a pick lands on the wrong field.

Predates slice 4 — but before slice 4 the user could not CREATE this entity
configuration. Now: select the scene entity (has `SpriteRenderer`) ->
+ Add Component -> `Arcane::PostProcess`. Two clicks. Exactly the composition
class a per-task review cannot see.

**Fix:** `PushID(ci.descriptor->hash)` around each component's header +
`visitFields`. Separately stop baking the mutable display string into the
asset-ref button id (use `###assetref` — only `###` resets the hash).

## MAJOR 3 — `EntityInfo` is add/removable, breaking the identity invariant it documents

`ComponentCatalog.cpp` hide-list vs `Components.hpp:99-112`.

`EntityInfo` documents "the Guid is generated when the component is added and is
the durable cross-save identity". Both real creation paths honour that
(`Guid::Generate()`). `Edit::AddComponent` default-constructs instead, so:
- Remove -> wipes name AND durable Guid for the whole selection.
- Add on a multi-selection -> stamps the SAME nil Guid on every entity.

Latent (nothing reads `EntityInfo::id` yet) but save-affecting, in the exact
field the ID/GUID-first direction depends on, reachable by a two-click gesture.

**Fix:** add `Arcane::EntityInfo` to the hide-list (the Outliner owns its
lifecycle via create/rename), or give descriptor-driven adds a post-construct
hook so it mints a fresh Guid per entity. Needs an explicit decision.

## MAJOR 4 — the no-project gate strands the editor's own Open Project UI

`ArcaneEditor/src/main.cpp` gate vs `EditorPanels.cpp:58` + `EditorApp.cpp:586`.

`File -> Open Project...` is shipped, wired, and explicitly designed to survive a
project-less state ("editor left with no plugin; user can Open another
project"). The gate removes the only cold-start path into it, and the Hub that
justifies the gate has not landed.

The a45cfa03 audit ("no tracked caller launches ArcaneEditor.exe") was true but
too narrow: `Arcane/scripts/launch.ps1` is untracked-but-present, uses
`Start-Process` with no `-NoNewWindow`, and `ArcaneEditor` is a ConsoleApp — so
`launch ArcaneEditor` now flashes a console and shows nothing at all.

**Fix:** either hold the gate until the Hub ships, or on bare launch boot and
immediately raise the existing Open Project dialog.

## MAJOR 5 — `EngineInfoJson` takes `argv[0]`: wrong value, and a crash on non-ASCII paths

`Loom/src/ProjectBoot.hpp`.

`argv[0]` is whatever the launcher put on the command line — the documented
workflow (`cd` into the dir, run `ArcaneEditor.exe`) yields a bare relative name,
useless to a Hub recording it. Worse, MSVC hands `main` an ANSI-codepage `argv`
and nlohmann's `dump()` is strict-UTF-8 with no `JSON_NOEXCEPTION`, so an install
under e.g. `C:\Users\Jose\...` with a non-ASCII byte throws out of `main` ->
terminate. That is the probe failing in exactly the way it exists to prevent.

**Fix:** don't use `argv[0]`. The engine already has `GetModuleFileNameW`
(`Runtime.cpp:43-51`); resolve the wide path and convert explicitly to UTF-8.
Add a non-ASCII path test.

---

## The Runtime roster comment is factually wrong (blast radius zero today)

`Runtime.cpp` claims "ComponentIDs come from `TypeID<T>` resolved BY HASH, not a
registration counter, so registration order cannot renumber anything or
invalidate an existing save." Verified false:

`TypeContext.hpp:93` — `const ComponentID id = m_next++;`. The hash is the KEY;
the id is a **monotonic counter in first-touch order**. Registration order fully
determines numbering, and this change shifts it (a plugin registering
Transform+SpriteRenderer had 0,1; now 0,3).

**Checked against the dev Astra at `D:/dev/starworks/astra/` (2026-07-26):**
- The counter is UNCHANGED there — `m_next++` still. So the id claim is wrong in
  both copies.
- BUT dev has **fixed the persistence half** (`Archetype.hpp`, marked "CR-4"): it
  now rebuilds the mask from hash-resolved descriptors instead of trusting the
  on-disk words, plus a popcount integrity check, and its comment states
  outright that "ComponentIDs are assigned per-run and are NOT stable across
  processes".
- The VENDORED copy has zero CR-4 and still does `make_unique<Archetype>(mask)`
  from the raw disk words.

So the cross-process persistence hazard is **already solved upstream** and lands
on the next Astra vendor sync. Nothing in this engine hits it today: every binary
blob is memory-only and same-process (Play snapshots, structural undo, hot
reload), and the only path-based `Save/Load` has two test callers.

**Action: rewrite the comment** to state the real invariant (ids are
process-local and counter-assigned; only the hash is stable), and note the
vendor-sync dependency. The change itself is correct and fixes two real bugs.

## The ABI tripwire is a tautology

Found independently by two reviewers. `EngineInfoJson` is an `inline` header
function reading `kGamePluginABIVersion`; the test includes the same header and
compares against `kGamePluginABIVersion`. Both sides expand to the same constant
in one TU — bumping it changes both. It **cannot** fail on an ABI bump.

What it does catch: a refactor that hardcodes a literal, a renamed key, a type
change. What it structurally cannot catch: a **stale built binary**, which is the
failure the design cares about. Only spawning the built exe and parsing stdout
catches that — which is what the Hub will do anyway.

**Action:** keep the test, correct its comment and the plan's Verification
Summary, and stop claiming it guards ABI bumps.

---

## Minors (land as follow-ups)

1. Structural edits refuse **silently** when a transaction is open — only an
   `ARC_WARN`. Presents as "the right-click menu stopped working". Disable the
   items on `undo.InTransaction()` or surface it.
2. `DrawAddComponentPopup` doesn't gate rows on `editMode`; a popup left open
   when Play starts stays interactive and then silently no-ops.
3. Int fields render `%.3f` in multi-select and round-trip `int32 -> float ->
   strtof -> int` (truncates, lossy past 2^24). Pass the kind into
   `MultiScalarRow`.
4. `AssetRef` mixed-value is computed then discarded — the one kind the "mixed
   shows blank" work set out to fix that didn't get it.
5. `kAddComponentPopup`'s comment states the opposite of the truth (the ids are
   per-window and DIFFER; the shared buffer is justified by one-popup-at-a-time,
   not by id equality).
6. The probe shares stdout with the spdlog console sink. Nothing logs on the
   clean path today, but the Hub's "read one line" contract is one stray
   `ARC_WARN` away from breaking. Route the log to stderr.
7. The probe reports the ABI compiled into the **exe**, not the loaded
   `Arcane.dll` where enforcement lives. A partially-updated install stamps a
   number the runtime rejects. Add `ARCANE_API uint32_t PluginABIVersion();`.
8. `Runtime`'s ctor now resolves ten `TypeID` statics at construction, so the
   FIRST `Runtime` in a process permanently pins Arcane.dll's ids — and the
   dangerous option is the **default argument** `externalContext = nullptr`.
   Currently clean (every test passes the shared context) but the guardrail is
   now load-bearing and enforced only by convention. Consider removing the
   default.
9. `m_componentNames` grows by 10 per boot and 10 per hot reload, never trimmed.

## Test-adequacy summary

- Well tested: the pure cores. `BuildComponentCatalog`'s missing-count rule is
  cross-validated against the mutator it predicts; the tag-component and
  dead-entity cases are real guards; the Runtime roster test genuinely
  reproduces the desk bug.
- **Vacuous:** `ClassifyField`'s Bool, Float and Vec3 arms can all be DELETED
  with the suite still green (the test only inspects `SpriteRenderer`, which has
  no bool and no vec3, and `CHECK(sawFloatOrInt)` is satisfied by an int alone).
- **Weak:** the "sticky" test's fixture `1,2,1` distinguishes OR-vs-assign but
  NOT seed-vs-previous — a previous-compare-with-assignment impl passes it. Add
  the `1,2,2` fixture.
- **Vacuous:** the forward-slash assertion feeds an already-forward-slashed
  path, so `string()` and `generic_string()` are identical. Feed a backslash.
- **Latent:** `MixedWorld::TransformHash()` returns 0 on lookup failure, which
  would make 5 of the 7 mixed-value tests pass trivially. Add
  `REQUIRE(hash != 0)`.
- Of the 8 behaviours the multi-select commit claims, ~1.5 are pinned. The gate
  is confirming the data layer beneath that feature, not the feature.

## What the reviewers explicitly cleared

- ImGui ID-stack balance in the new code is **clean** — `PushMultiItemsWidths`
  pushes exactly `count` and `MultiScalarRow` pops exactly `count`; every
  push/pop pair is on an unconditional path; the `continue`s all precede pushes.
- `BeginPopupContextItem()` with a null str_id on a CollapsingHeader is correct.
- The `pendingRemove` deferral is sound; descriptor pointers ARE stable.
- The `addComponentPending` latch has no reachable wedge.
- `RegisterComponent<T>` IS idempotent; no mask-width change; no MAX_COMPONENTS
  risk (10 of 128); scene JSON unaffected; Runtime-before-plugin ordering is
  genuinely guaranteed; `ReRegisterComponent` still rebinds for hot reload; no
  ABI bump was warranted.
- `d372e457` (per-instance outline latch) is clean and can ship as-is.
