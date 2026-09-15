# Thumbnail golden coverage — decision record (2026-09-15)

**Status:** decision. Synthesizes two research passes done the same day:
`2026-09-15-thumbnail-golden-internal-research.md` (this repo's directives and machinery) and
`2026-09-15-thumbnail-golden-external-research.md` (Unreal from the local dump, Godot, Unity).
Read those for the evidence; this note records what was decided and why.

**Trigger:** F2c Plan 2's final review found three Critical stale-geometry bugs in the thumbnail
and preview paths that thirteen task reviews and both golden lanes missed
(`docs/plans/2026-09-10-f2c-mesh-import-plan2-closeout.md`, C1–C3). Debt 8 there asks for a
second imported mesh in the golden scene so the lanes can catch that class again. The Task 12
review had already noted the mesh row is scrolled out of `editor-ui.png`'s 1280×720 capture — so
the editor lane cannot see any thumbnail today, and a second mesh alone would not change what the
gate judges.

---

## 1. The decision

**Pin thumbnails as their own artifact class, in-process, at the harvester's own 64×64 offscreen
surface — not by scrolling the editor shell into a position where a shell capture happens to
include them.** Concretely: `ImageCompare` assertions at budget 0 against committed references
under `ReferenceProject/Verify/References/thumbs/`, added to the existing `[gpu][thumbs]`
machinery (`ArcaneTests/src/MeshThumbnailHarvestTest.cpp`), with a re-bless mode shipped in the
same change.

Why this and not the three alternatives argued in chat:

- `editor-ui.png` is a **shell** capture and a **shared** dx12/vulkan slot, re-blessed every time
  a panel or default layout moves. F4 (editor camera, 2D↔3D viewport, gizmos) and the
  outliner/inspector arcs will move it repeatedly. Coupling a per-asset rendering property to
  panel geometry means every layout re-bless is a moment a thumbnail regression can be laundered
  into the new reference — the exact failure Task 12 spent three rounds avoiding.
- A second sectioned prop in the scene adds near-zero information to a runtime lane that already
  covers per-section draws.
- The existing two-mesh `[gpu][thumbs]` test proves "two harvests differ", not "the harvest is
  right".

The internal research adds the reason this is the *right layer*, not just a convenient one: the
host-witness spec's own rule (`docs/specs/2026-09-03-host-witness-harness-design.md` §2) sends
boot/CLI/settle/report questions to the real host and render-path questions to an in-process
case against the real graph context. "Does this asset's offscreen render still match" is a
render-path question, and the harvester needs no host boot to answer it — the existing test
already constructs it from a bare `OffscreenVehicle` plus hand-filled `Services`.

The external research adds the honest context: **no engine surveyed golden-tests thumbnails as
their own class** — Unreal built `ImageComparer` for whole-level screenshots and never connected
it to `FObjectThumbnail`; Godot has no golden framework at all; Unity's is fixed at whole-camera
512×512 captures. Nothing about a 64px image makes it harder to pin; the absence of precedent
reads as "nobody's thumbnail renderer was volatile enough to bother". Ours is about to be: the
north star (`docs/research/2026-09-14-engine-ceiling-deadlock-and-box3d.md` §2/§5) queues T1's
GGX cutover and five more renderer arcs, each of which moves every lit thumbnail pixel.

That last fact is the first-order design constraint, and the reason a bless mechanism is not
optional: the template this copies, `GoldenImageTest.cpp`'s lit cube, **has never been
re-blessed since it was hand-authored on 2026-08-26 and has no mechanism to be.**

## 2. The shape — six decisions

1. **Home:** extend `MeshThumbnailHarvestTest.cpp` (and a material sibling), do not open a
   parallel `[gpu][golden]` file. One home for "the harvester renders the right pixels for the
   right guid"; the control/subject cross-contamination check and the reference compare sit
   side by side over the same harvester.
2. **References:** `ReferenceProject/Verify/References/thumbs/<subject>-<name>.png`, resolved
   with the existing `ReferenceImages.hpp` rule (`thumbs/<backend>/<name>.png` overrides
   `thumbs/<name>.png`), so backend divergence gets a backend override exactly as
   `runtime-scene` does today. Initial set — the minimum that would have caught C1–C3 and covers
   each `Subject` kind that can collide: `mesh-golden_prop` (imported, multi-section, blue/red),
   `mesh-reference_cube` (primitive), `material-reference_mesh` (mesh-kind material sphere), and
   one material of each other surface the harvester serves (sprite, fullscreen) so the
   material path is pinned too. Named by subject and asset, not by guid: a human reads the diff.
3. **Tolerance:** budget 0, the library default, the same as every lane and the lit cube. Any
   need for slack is a determinism bug to fix, not a knob to raise
   (`ImageCompare.hpp`'s own comment, and `feedback_default_values_are_not_measurements`).
   **Gate on D3D12 like the lit cube** (`ARC_REQUIRE_BACKEND`): one committed set, one backend
   in CI; backend parity remains the host lanes' question.
4. **Bless:** an explicit opt-in switch on the test — `ARCANE_THUMBS_BLESS=1` checked once at the
   top of each thumbnail case — that writes the harvested bytes over the reference **at the level
   it resolved from** (mirroring the host's `--bless` rule: never a bulk overwrite that flattens a
   backend override) instead of comparing. Scope is the Catch2 filter: one case name re-blesses
   one image; `[thumbs]` re-blesses the set. The review step is `git diff --stat` plus the
   diff/actual PNGs the compare path writes under `Saved/Verify/thumbs/` on failure. This is the
   external report's "reuse the one-image write primitive, automate the loop" and the internal
   report's "set the var, run the filter once, eyeball, commit" — the same thing. After each
   renderer arc the re-bless is one command, reviewed, not N host launches.
5. **Capture the harvester's own surface**, never a crop of a panel screenshot — the invariant
   the external report names from UE's widget-capture API; Arcane is already there
   (`MaterialPreviewHarvester` renders into its own 64×64 `NriGraphContext`). Stated so nobody
   "improves" it later.
6. **Not content-hash-keyed.** The external report's Godot-derived suggestion (key the reference
   by a hash of the pixel-determining inputs) is the right idea for the harvester's **runtime
   cache** — `PrimeFromDisk`'s mtime-only check re-harvests on every checkout/CI clone that bumps
   mtimes without changing bytes — but it buys nothing for a golden *test*: at budget 0, a
   refactor that leaves pixels unchanged already passes, and an asset that changes already fails
   loudly, which is the signal. Adopting it for references would turn "the asset changed" from a
   red compare into a silent "missing reference" refusal. **Ruling: references keyed by name;
   the content-hash tiebreak goes to the harvester cache as its own follow-on** (§4).

## 3. What this does not close, said plainly

The internal report's strongest objection stands and is accepted: this proves the **harvester's
pixels in isolation**. It does not exercise the end-to-end editor path — `PollAssetWatch`
staleness, cook completion → `InvalidateMesh` re-arm (Task 12a's "never desk-exercised live"
caveat), the Browser row itself. Debt 8's literal ask (a second imported mesh in the golden
scene) would exercise the runtime lane a second time but still not the thumbnail path, because
the editor capture cannot see the rows. So Debt 8 is **re-shaped**, not closed by fiat:

- the harvester's correctness → this note's golden set (closes C1's bug class at the layer it
  lives);
- the end-to-end editor thumbnail path → a named, deferred **witness scenario**: the real editor
  headless with `--report` extended by a thumbnail census (count, per-guid content hash,
  harvest state), graded from JSON like every other lane. That is the introspection direction's
  own instrument and the only honest way to prove the live path; it needs a report-schema
  extension (`VerifyReport.hpp`, `automation-vocabulary.txt`, the gate's anti-drift pin) and is
  its own small arc, not a ride-along on this one.

## 4. Follow-ons this note names (owners: the next spec that touches the area)

- **Harvester cache: content-hash tiebreak** beside the mtime check in `PrimeFromDisk`
  (Godot's two-stage rule) — kills spurious re-harvests on checkout/CI clone.
- **Thumbnail census in `--report`** → the witness scenario above.
- **The parked cvar arc:** `kThumbSize`, `kThumbTime`, and the mesh light/ambient triple are
  `constexpr` today; the day any becomes a cvar, the whole reference set re-blesses. Add to that
  arc's trigger list.
- **Spike before implementation:** `ImageCompare`'s 31×31 SSIM antialiasing window has never
  been measured at 64×64 (it covers ~a quarter of the image). A throwaway probe — two identical
  harvests, two differing by one section colour — decides whether budget-0 behaves at this size
  before any reference is committed.

## 5. What changes in the debts arc

Debt 8 is replaced by the golden set above (plus the spike). Debts 1 (staging mirrors deletions
for `Content/`, `Source/`, `Verify/` — premake-native `{RMDIR}` before `{COPYDIR}`, which also
survives the Linux port where `robocopy` would not), 12+13 (`DebugFailNextUpload` instrument +
the `[gpu][meshcache]` refusal test, giving `uploadRefused` its reader), 14 and 15 are unchanged.
Order: 13+12 → 14/15 → 1 → spike → thumbnail golden set. No push until the user says so.
