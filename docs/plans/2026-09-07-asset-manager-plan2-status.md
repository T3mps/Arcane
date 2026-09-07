# Asset Manager Plan 2 — Reference Index + Status Lens Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land spec §3.3 (scene reference manifest, schema v4) + §9 (editor
`AssetReferenceIndex`, `AssetActivityLog`, the Status lens dashboard) and take the
bottom-bar digest's `unused` count live.

**Architecture:** The scene serializer emits a save-time `"assets"` manifest as a
byproduct of the reflected-field visit it already makes (schema v3→v4, additive —
v3 scenes keep loading; the loader never reads the manifest);
`ListAssetReferences` reads the manifest for v4 scenes and keeps the Plan-1
structural scan as the pre-v4 fallback. Editor-side, a pure `AssetReferenceIndex`
(forward map · inverted map · inbound counts · tombstones, UE's
remove-before-readd discipline) lives inside `AssetPanelModel` and is fed by the
very same `refsFor` answers the model already fetches per rebuilt guid — zero
extra parses. A session-only `AssetActivityLog` ring is pushed from the existing
event seams. The Status lens is thin ImGui (new `StatTile`/`MeterBar`/
`CardFrame`/`TimelineFeed` widgets) over `HealthCounts` + the index + the log.

**Tech Stack:** C++23, ImGui 1.92.9 (vendored), nlohmann/json, Catch2
(ArcaneTests), premake5/msbuild (VS18).

**Spec:** `docs/specs/2026-09-06-asset-manager-redesign-design.md` — read it
first; this plan implements its §3.3, §9, the Plan-2 rows of §11.1, and §12's
Plan-2 test obligations. §17 records what Plan 1 actually landed (including four
2026-09-07 revisions) — the panel this plan extends is §17's panel, not §5/§6's
original text alone.

## Global Constraints

- Build: `Arcane.slnx` via VS18 msbuild
  (`C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`),
  never a bare `.vcxproj`; `ARCANE_SDK` may be stale in the process env — set
  per-invocation if needed.
- Tests: run `ArcaneTests.exe` FROM the exe dir
  (`bin\Debug-windows-x86_64-md\ArcaneTests\`); capture the Catch2 seed banner;
  `~[gpu]` for baseline-comparable runs. Never construct a bare `Arcane::Runtime`
  in a test.
- ABI: this plan bumps `kGamePluginABIVersion` 22→23 once (Task 1). The bump is
  **documentary** — no vtable/layout change — per the v16 precedent (the last
  scene-format move rode a bump, `PluginABI.hpp:269`) and the standing
  "ABI bumps are cheap" rule. `ReferenceProject.arcproj` restamps in the same
  commit (the unbroken v17–v22 precedent).
- Editor sources are globbed by premake — new `ArcaneEditor/src` files need no
  premake edit. **ArcaneTests files are explicit** — every new test TU AND every
  new pure editor TU it exercises must be added to the ArcaneTests `files{}`
  block in `premake5.lua` (the `AssetPanelModel.cpp` pattern, `premake5.lua:826-832`),
  then `GenerateProjects` re-run.
- Fidelity: spec §11.2 values are verbatim requirements (stat tile number 24px
  `PushFont`, meter bar 10–12px with 2px surface gaps, feed dots 7px, pills 12px
  text / 16px line / 1px `#333333`, attention frame `#7a5a20`, amber `#ffa61a` =
  `Theme::kAmber`). All chrome colors are `EditorTheme` tokens. Amber appears on
  the refused tile's ICON only — numbers stay in text tokens.
- Every ImGui table: `ImGuiTableFlags_NoSavedSettings`.
- **Visual keystone:** `.superpowers/design/asset-manager-mockups/README.md` —
  `renders/OptionE-Status-FINAL.png` is the Status redline;
  `Interactions-FINAL.png` binds hover/click for Status surfaces too; the Demo
  board binds behavior (Recook flips refused→queued live; Focus-in-Graph stays
  for Plan 3). Any task that draws UI ends with a structural comparison against
  the matching render (spec §11.2 is the numeric arbiter — never a pixel diff).
- Commit after every task (message prefixes as shown); do NOT push — the push
  follows the user's desk pass (Task 9).

## Design rulings pinned by this plan (argue with the plan, not the executor)

1. **Scene version gates widen to accept {3, 4}** at the two scene-file gates
   (`SceneSerializer.hpp:255-257` LoadJson, `SceneAsset.hpp:99-111` ReadSceneFile).
   All three gates are exact-equality today; a naive bump would brick every v3
   scene. v1/v2 stay rejected (no upgrade path, unchanged). The **entity-clipboard
   gate stays exact** (`EntityOps.cpp:434-448`) — clipboard payloads are
   transient and in-process; writer and reader always share one constant.
2. **The manifest collector is a byproduct of the reflected-field visit**
   (spec §3.3 "byproduct, not a second pass"): `ReflectionJsonWriter` gains an
   optional guid sink. Guid detection is the **same {hi,lo} shape rule the
   structural scan uses** (`Assets.cpp:167-169`) applied to the just-built nested
   object — proven, and it makes manifest-vs-scan equivalence exactly provable
   (manifest = scan candidates minus the resolvability filter).
3. **The manifest fast path applies NO resolvability filter.** A dangling target
   must be reported so the index can tombstone it (§9.1); the structural scan
   keeps its filter because for a shape heuristic it is the false-positive
   killer (§3.4). Non-scene formats already report dangling refs unfiltered.
4. **A v4 scene missing its `assets` key falls back to the structural scan**
   (defensive; a hand-stripped manifest degrades to Plan-1 behavior, never to
   an empty answer). The serializer itself always emits the key, even empty.
5. **`IsIdentityGuidFieldName` is promoted to one shared engine header**
   (`Arcane/Serialization/IdentityFieldRule.hpp`), retiring the hand-synced
   copy hazard `Assets.cpp:111-118` itself flags. The editor's
   `Arcane::Editor::IsIdentityGuidFieldName` keeps its symbol (InspectorView +
   tests use it) and delegates.
6. **The index lives inside `AssetPanelModel`**, fed during `RebuildIfDirty`
   from the same `refsFor` answers the model already fetches for every rebuilt
   guid (proven by the Plan-1 call-count tests) — one parse serves both. Full
   rebuild = `Clear()` + re-feed; per-guid rebuild = incremental `Update`.
   The index itself stays a separate pure unit with its own tests.
7. **"Unused" is the model's policy, not the index's**: eligible kinds are
   exactly {Texture, Material, Sprite, Mesh} (§9.1); the index only serves
   inbound counts. Inbound counts ALL edge kinds — `DerivesFrom` is inbound too,
   which is what makes the §9.1 cascade work (a dead minted sprite flags first;
   its texture flags after the sprite is removed).
8. **Recook = invalidate + erase the refusal row + queue + dirty**:
   `InvalidateArtifact(guid)` + `m_cookDiagnostics.erase(guid)` +
   `PublishCookDiagnostics()` + `CookQueue::NoteChanged()` +
   `MarkDirty(guid)`. Erasing the row is what flips Refused→Queued honestly
   (`IsCookPending` presumes pending on absence, `EditorAppProject.cpp:732-747`);
   a still-failing source re-fails and the row comes back. This matches the Demo
   board's bound behavior. There is no per-guid cook API — `NoteChanged()` is
   whole-project, coalescing, and correct here.
9. **"Problems" jumps to the pane, nothing more** (spec §9.2 verbatim):
   `m_panelVis.visible[Problems] = true` + `SelectDockTab("Problems")` — the
   Edit→Rename/Outliner precedent (`EditorAppFrame.cpp:2167-2173`). No
   pre-filtering (not specified; do not invent).
10. **"Reveal" is panel-internal**: clear search + kind filter (so the row is
    guaranteed visible), `state.lens = Browse`, `model.Select(guid)` — the
    selection-stamp machinery does the scroll-once.
11. **Activity timestamps are `std::chrono::steady_clock`** snapshots rendered
    as ages ("just now" / "N min ago" / "N h ago"). Session-only, cleared on
    project switch. `Deleted` exists in the vocabulary but has **no live
    producer** (there is no delete flow in the editor; the registry has no
    Remove API) — recorded, not invented.
12. **The queued card's progress strip is derived, not fabricated**: fraction =
    `cooked / (cooked + queued)` from `HealthCounts` (the same numbers the meter
    shows). No fake per-asset progress.
13. **Dangling-reference reporting is data-only in this plan**: the index
    exposes `DanglingTargets()` (tested), but the Status lens draws only what
    §9.2/OptionE binds — no dangling card is invented. Surfacing it is a
    recorded follow-up.

---

### Task 1: Engine — shared identity rule, v4 scene manifest emission, widened load gates, ABI 23

**Files:**
- Create: `ArcaneClient/src/Arcane/Serialization/IdentityFieldRule.hpp`
- Modify: `ArcaneClient/src/Arcane/Serialization/ReflectionJson.hpp` (writer ctor + nested-struct branch, `:330-398`)
- Modify: `ArcaneClient/src/Arcane/Serialization/SceneSerializer.hpp` (`kSceneJsonVersion` `:62`, `SaveJson` `:79-157`, load gate `:253-257`, version-history comment `:41-61`)
- Modify: `ArcaneClient/src/Arcane/Serialization/SceneAsset.hpp` (`ReadSceneFile` version gate/message, `:99-111`)
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.cpp` (delete the TU-local `IsIdentityGuidFieldName` `:119-125`, include the shared header)
- Modify: `ArcaneEditor/src/Panels/AssetPanelModel.hpp` (`:207-213` delegates to the shared rule)
- Modify: `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` (`:581` 22→23 + v23 ledger entry after the closed v22 entry `:524-580`)
- Modify: `ReferenceProject/ReferenceProject.arcproj` (`engine.abi` 22→23, `:6`)
- Test: `ArcaneTests/src/SceneAssetTest.cpp` (new cases), `ArcaneTests/src/SceneJsonTest.cpp` (version-gate cases)

**Interfaces:**
- Consumes: `SaveJson(const Astra::Registry&)` (`SceneSerializer.hpp:79`),
  `ReflectionJsonWriter` (`ReflectionJson.hpp:330`), `SaveSceneFile`
  (`SceneAsset.cpp:19`), `Arcane::Guid` (`ToString`, `IsValid`), the
  `{hi,lo}` guid registration (`Components.hpp:350-353`).
- Produces: `Arcane::IsIdentityGuidFieldName(std::string_view)` in the shared
  header; `ReflectionJsonWriter(nlohmann::json& out, std::vector<Arcane::Guid>* assetGuidSink = nullptr)`;
  scenes saved with `"version": 4` and a top-level `"assets": [sorted guid strings]`;
  loaders that accept versions 3 and 4. Task 2 relies on the manifest key being
  named exactly `"assets"` and holding canonical guid strings.

- [ ] **Step 1: Write the failing tests.**

In `SceneAssetTest.cpp` (reuse its `Fixture` registry-builder at `:28-50` and
`TempDir` helper at `:52-59`; extend the fixture registry so at least one
component carries an asset guid — e.g. give an entity an
`Arcane::PostProcess{ material = <known guid> }` and a second entity a component
referencing the SAME guid, plus one nil-guid field):

```cpp
TEST_CASE("SaveSceneFile emits a v4 assets manifest: distinct, sorted, identity-excluded, nil-dropped", "[scene][json]")
{
    // registry with: guid X referenced by TWO components (dedup), Identity ids
    // (excluded by field name), one nil guid field (dropped)
    ...build registry...
    const fs::path file = TempDir() / "manifest.arcscene";
    std::string err;
    REQUIRE(Arcane::SaveSceneFile(file, reg, sceneId, &err));

    std::ifstream in(file);
    const nlohmann::json doc = nlohmann::json::parse(in);
    REQUIRE(doc["version"].get<int>() == 4);
    REQUIRE(doc.contains("assets"));
    REQUIRE(doc["assets"].is_array());
    REQUIRE(doc["assets"].size() == 1);              // X once, despite two mentions
    CHECK(doc["assets"][0].get<std::string>() == X.ToString());
}

TEST_CASE("A v3 scene still loads after the v4 bump", "[scene][json]")
{
    // write a scene body with a LITERAL "version": 3 (not the symbolic constant)
    // and load it through ReadSceneFile/LoadJson -- must succeed.
}

TEST_CASE("The loader never reads the assets manifest", "[scene][json]")
{
    // save a scene, then hand-corrupt its manifest ("assets": ["garbage", 42])
    // and reload -- entities load intact; the corruption is invisible to the loader.
}

TEST_CASE("An empty scene still emits an (empty) assets manifest", "[scene][json]")
{
    // registry with entities but zero asset refs -> "assets": [] present.
}
```

In `SceneJsonTest.cpp`: the existing rejects-mismatch case (`:307-324`, uses
`kSceneJsonVersion + 999`) keeps passing untouched; add one case asserting a
literal `"version": 3` document is ACCEPTED by `LoadJson` and a literal
`"version": 2` is refused. The `CHECK(doc["version"] == kSceneJsonVersion)` at
`:354` moves symbolically with the constant — verify it still passes, don't edit
it.

- [ ] **Step 2: Run the new tests to verify they fail.**

Run: `ArcaneTests.exe "[scene]" --rng-seed time` from the exe dir.
Expected: the four new cases FAIL (no `assets` key; v3 gate untouched passes —
the accept-3 case only goes red after the bump, which is fine: write it, watch
the manifest cases fail now, and re-check the full set after Step 3).

- [ ] **Step 3: Implement.**

3a. `IdentityFieldRule.hpp` (new, header-only):

```cpp
#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace Arcane
{
    // The ONE identity-field rule (spec s3.3): a guid field named "id" or
    // "guid" (case-insensitive) is an identity, not an asset reference.
    // Formerly three hand-synced copies (engine scene scan, editor panel
    // model, via InspectorView) -- this header is now the single source; the
    // editor's Arcane::Editor::IsIdentityGuidFieldName delegates here.
    inline bool IsIdentityGuidFieldName(std::string_view fieldName)
    {
        std::string lower(fieldName);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower == "id" || lower == "guid";
    }
}
```

Delete `Assets.cpp:119-125` (and its `:111-118` mirror-hazard comment), include
the new header, and let the call site at `:171-172` resolve to `Arcane::`.
Change `AssetPanelModel.hpp:207-213` to a delegating inline (keep the
`Arcane::Editor` symbol and its doc comment; body becomes
`return Arcane::IsIdentityGuidFieldName(fieldName);`). `AssetBrowserTest.cpp:44-59`
keeps passing unmodified — that is the proof the promotion is behavior-identical.

3b. `ReflectionJsonWriter` gains the sink (`ReflectionJson.hpp`):

```cpp
class ReflectionJsonWriter : public Astra::IFieldVisitor
{
public:
    explicit ReflectionJsonWriter(nlohmann::json& out,
                                  std::vector<Arcane::Guid>* assetGuidSink = nullptr)
        : m_out(out), m_assetGuidSink(assetGuidSink) {}
    ...
private:
    std::vector<Arcane::Guid>* m_assetGuidSink = nullptr;
```

In the nested-reflected-struct branch (`:362-378`): construct the sub-writer
with the SAME sink (`ReflectionJsonWriter subWriter(sub, m_assetGuidSink);`) so
depth-2+ guids still collect, and after `sub` is built, apply the shape rule
(the exact test `ScanSceneJson` uses, `Assets.cpp:167-169`):

```cpp
if (m_assetGuidSink && !Arcane::IsIdentityGuidFieldName(field.name)
    && sub.is_object() && sub.size() == 2
    && sub.contains("hi") && sub.contains("lo")
    && sub["hi"].is_number_unsigned() && sub["lo"].is_number_unsigned())
{
    const Arcane::Guid g(sub["hi"].get<std::uint64_t>(),
                         sub["lo"].get<std::uint64_t>());
    if (g.IsValid())
        m_assetGuidSink->push_back(g);   // dedup happens at emission
}
```

NOTE for the executor: check `Guid`'s actual two-u64 constructor spelling
(`Assets.cpp:137-140`'s `GuidFromHiLo` shows the working idiom — mirror it).
The clipboard (`SerializeSubtrees`) and every other existing writer caller pass
no sink and are untouched.

3c. `SaveJson` (`SceneSerializer.hpp`): bump the constant, collect, emit.

```cpp
inline constexpr int kSceneJsonVersion = 4;
// v4 accepts v3 on load (additive change); v1/v2 stay rejected.
inline constexpr int kSceneJsonVersionMin = 3;
```

In `SaveJson`: declare `std::vector<Arcane::Guid> sceneAssets;` before the
entity loop; construct the per-component writer (`:126`) as
`ReflectionJsonWriter writer(cj, &sceneAssets);`. After the entity loop,
always emit (Ruling 4):

```cpp
std::unordered_set<Arcane::Guid> seen;
std::vector<std::string> manifest;
for (const Arcane::Guid& g : sceneAssets)
    if (seen.insert(g).second)
        manifest.push_back(g.ToString());
std::sort(manifest.begin(), manifest.end());   // deterministic diffs
doc["assets"] = manifest;
```

Load gate (`:253-257`): accept the range —
`const int v = vit->get<int>(); if (v < kSceneJsonVersionMin || v > kSceneJsonVersion) return false;`
The loader reads nothing else new — it never touches `"assets"`.
Extend the `:41-61` version-history comment with a dated v4 entry (additive
manifest, loader-blind, spec §3.3).

3d. `SceneAsset.hpp:99-111` (`ReadSceneFile`): widen the same way, keeping the
message sentence shape ("…is scene schema version N; this engine reads 3
through 4"). `EntityOps.cpp` is NOT touched (Ruling 1).

3e. `PluginABI.hpp`: `kGamePluginABIVersion` 22→23 (`:581`) + a v23 ledger
entry after `:580` recording: scene file format v3→v4 (the first format move
since v16); no vtable/layout/API change — documentary bump per the v16
precedent and the cheap-bumps rule; measured: zero serializer symbol usage in
`ReferenceProject/Source` AND `Gacha/Game/Source` (both grepped for
`SaveJson|SceneSerializer|SaveSceneFile|kSceneJsonVersion|SerializeSubtrees|LoadJson|ReadSceneFile`);
v3 scenes remain loadable. Restamp `ReferenceProject.arcproj` to 23 in the same
commit.

- [ ] **Step 4: Build + run the affected suites.**

Run from the exe dir: `ArcaneTests.exe "[scene],[json],[editor],[assets]" --rng-seed time`
Expected: all green — including the untouched `AssetBrowserTest` identity-rule
cases, `EntityClipboardTest` (symbolic stamps move together), and `HostBootTest`
fixtures (they stamp `kSceneJsonVersion` symbolically → now write 4 with no
manifest → the loader doesn't care). Then one full `~[gpu]` Debug run.

- [ ] **Step 5: Commit** —
`feat(engine): scene schema v4 -- save-time assets manifest, shared identity rule, ABI 23`

---

### Task 2: Engine — manifest fast path in `ListAssetReferences`

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.cpp` (the `.arcscene` branch, `:946-954`; contract comment `Assets.hpp:321-347` gets a manifest paragraph)
- Test: `ArcaneTests/src/AssetReferencesTest.cpp`

**Interfaces:**
- Consumes: Task 1's manifest (`"version" >= 4`, `"assets"` array of guid
  strings), `ParseJsonUncached` (`Assets.cpp:482`), `ScanSceneReferences`
  (`Assets.cpp:199`), `Guid::FromString`/`IsValid`.
- Produces: `ListAssetReferences` for a v4 scene returns the manifest verbatim
  as `AssetRefKind::References` — **including unresolvable targets** (Ruling 3).
  Task 3's tombstones depend on dangling targets being reported.

- [ ] **Step 1: Write the failing tests** (same `WriteFile` + resolver-lambda
pattern as the rest of the file; guid prefix `7e5a0001-…`):

```cpp
TEST_CASE("ListAssetReferences reads a v4 scene's manifest verbatim, including unresolvable targets", "[assets]")
{
    // v4 scene file whose manifest names X (resolvable) and Y (NOT in the resolver)
    // -> BOTH come back as References. Proves: fast path taken, no resolvability
    // filter (the structural scan would have dropped Y and, with no {hi,lo}
    // bodies present, found nothing at all).
    const auto scene = WriteFile(dir, "v4.arcscene", R"({
        "assets": ["7e5a0001-0001-4001-8001-00000000000a",
                   "7e5a0001-0001-4001-8001-00000000000b"],
        "entities": [], "id": "...", "version": 4})");
    ...
    REQUIRE(refs->size() == 2);
}

TEST_CASE("A v4 scene missing its manifest falls back to the structural scan", "[assets]")
{
    // "version": 4, no "assets" key, one {hi,lo} component ref -> scan finds it.
}

TEST_CASE("Manifest entries that are malformed or nil are skipped without hanging", "[assets]")
{
    // "assets": ["not-a-guid", "", 42, "<nil guid string>", "<valid>"] -> exactly
    // the valid entry returns. Malformed strings must be rejected BEFORE any
    // parse that could hit the known Guid-parse-on-invalid-hex hang.
}

TEST_CASE("Manifest and structural scan agree on the same scene (equivalence, spec s12)", "[assets]")
{
    // Build a real registry (SceneAssetTest's Fixture pattern), SaveSceneFile it
    // (writes v4 + manifest), point the resolver at it -> refs A.
    // Copy the file, strip "assets", rewrite "version" to 3 -> refs B (scan path;
    // every target resolvable in the fixture).
    // REQUIRE(set(A) == set(B)).
}
```

- [ ] **Step 2: Run to verify the new cases fail** (`ArcaneTests.exe "[assets]"`
from the exe dir). The existing scene-scan case (`:379`, literal `"version": 3`)
must KEEP passing before and after — it now pins the fallback path.

- [ ] **Step 3: Implement** — replace the `.arcscene` branch body:

```cpp
if (ext == ".arcscene")
{
    // v4 fast path (spec s3.3): the save-time manifest is exact -- no shape
    // heuristics and NO resolvability filter (a dangling target must surface
    // so the editor's index can tombstone it, spec s9.1). The structural scan
    // below keeps its filter: for a shape heuristic it is the false-positive
    // killer (spec s3.4).
    const auto vit = json->find("version");
    const auto ait = json->find("assets");
    if (vit != json->end() && vit->is_number_integer() && vit->get<int>() >= 4
        && ait != json->end() && ait->is_array())
    {
        std::vector<AssetRef> out;
        std::unordered_set<Guid> seen;
        for (const auto& entry : *ait)
        {
            if (!entry.is_string())
                continue;
            const std::string& s = entry.get_ref<const std::string&>();
            if (!IsCanonicalGuidString(s))   // shape-gate BEFORE FromString --
                continue;                    // the invalid-hex parse hang is a
                                             // known engine bug (backlogged)
            const Guid g = Guid::FromString(s);
            if (g.IsValid() && seen.insert(g).second)
                out.push_back({ g, AssetRefKind::References });
        }
        return out;
    }
    // pre-v4, or a v4 file whose manifest was hand-stripped: structural scan.
    auto resolvable = [this](const Guid& g)
    { return ResolveId(AssetId::FromGuid(g)).has_value(); };
    return ScanSceneReferences(*json, resolvable);
}
```

`IsCanonicalGuidString` is a small file-local helper beside `LowerExt`
(`Assets.cpp:86`): length 36, dashes at 8/13/18/23, hex elsewhere. NOTE for the
executor: verify `Guid::FromString`'s exact spelling and what it returns on
garbage (the reproducer in the Plan-1 task-4 report documents the hang class) —
the shape gate must make the question moot.

- [ ] **Step 4: Build; run `[assets]` + `[scene]`; then a full `~[gpu]` Debug run.**
Expected: all green.

- [ ] **Step 5: Commit** —
`feat(engine): ListAssetReferences reads the v4 scene manifest (structural scan demoted to pre-v4 fallback)`

---

### Task 3: Editor — `AssetReferenceIndex` (pure unit)

**Files:**
- Create: `ArcaneEditor/src/Panels/AssetReferenceIndex.hpp`
- Create: `ArcaneEditor/src/Panels/AssetReferenceIndex.cpp`
- Modify: `premake5.lua` (ArcaneTests `files{}`: add the new `.cpp` + its test TU, the `AssetPanelModel.cpp` pattern at `:826-832`), then re-run `GenerateProjects`
- Test: `ArcaneTests/src/AssetReferenceIndexTest.cpp` (new)

**Interfaces:**
- Consumes: `Arcane::AssetRef` / `AssetRefKind` (`Assets.hpp:47,53`) — nothing
  else; the unit is engine-facade-free and ImGui-free.
- Produces (Task 4 calls exactly these):

```cpp
namespace Arcane::Editor
{
    // Pure reference topology over ListAssetReferences answers (spec s9.1).
    // Policy-free: "unused" (kind-gated) is the model's rule, not this unit's.
    class AssetReferenceIndex
    {
    public:
        struct Node
        {
            std::vector<Arcane::AssetRef> outbound;  // last-known-good (spec s3.2)
            std::vector<Arcane::Guid>     inbound;   // referencers, sorted, unique
            bool exists = false;   // false + inbound nonempty == tombstone
        };

        void Clear();
        // Re-walk one asset with a fresh provider answer.
        //   refs == nullopt -> keep last-known-good outbound; inbound untouched.
        //   exists == false -> asset gone: removal pass over its outbound, then
        //                      the node survives only as a tombstone (if referenced).
        void Update(const Arcane::Guid& id, bool exists,
                    const std::optional<std::vector<Arcane::AssetRef>>& refs);

        int InboundCount(const Arcane::Guid& id) const;   // 0 if unknown
        const Node* Find(const Arcane::Guid& id) const;   // nullptr if unknown
        std::vector<Arcane::Guid> DanglingTargets() const; // tombstones w/ referencers
        std::size_t NodeCount() const;

    private:
        void RemoveOutboundEdges(const Arcane::Guid& source);
        std::unordered_map<Arcane::Guid, Node> m_nodes;
    };
}
```

- [ ] **Step 1: Write the failing tests** (headless, no registry, no temp dirs —
guids via `Guid::FromString` literals). Cases, each its own `TEST_CASE`, tag
`"[editor]"`, names in the `"AssetReferenceIndex <behavior>"` convention:

```cpp
TEST_CASE("AssetReferenceIndex builds forward, inverted and inbound counts", "[editor]")
{
    AssetReferenceIndex idx;
    idx.Update(A, true, Refs({ {B, References} }));
    idx.Update(C, true, Refs({ {B, DerivesFrom} }));
    idx.Update(B, true, Refs({}));
    CHECK(idx.InboundCount(B) == 2);                 // both edge kinds count
    REQUIRE(idx.Find(B));
    CHECK(idx.Find(B)->inbound == std::vector<Arcane::Guid>{ /*sorted*/ A, C });
}

TEST_CASE("AssetReferenceIndex re-walk removes old edges before re-adding (UE discipline)", "[editor]")
{
    // A->B, then A re-walks to ->C. Without the removal pass B keeps A forever
    // (the classic incremental-index corruption, spec s9.1).
    idx.Update(A, true, Refs({ {B, References} }));
    idx.Update(B, true, Refs({}));
    idx.Update(A, true, Refs({ {C, References} }));
    CHECK(idx.InboundCount(B) == 0);
    CHECK(idx.InboundCount(C) == 1);
}

TEST_CASE("AssetReferenceIndex re-walk with identical refs is idempotent", "[editor]")
// A->B twice -> B.inbound holds A exactly once.

TEST_CASE("AssetReferenceIndex keeps last-known-good on nullopt (spec s3.2)", "[editor]")
// A->B; Update(A, true, nullopt) -> InboundCount(B) still 1; A's outbound intact.

TEST_CASE("AssetReferenceIndex tombstones an unresolvable target and reports it dangling", "[editor]")
// A -> M (M never walked): Find(M)->exists == false, inbound {A};
// DanglingTargets() == {M}.

TEST_CASE("AssetReferenceIndex garbage-collects a tombstone when its last referencer lets go", "[editor]")
// ...then Update(A, true, Refs({})) -> Find(M) == nullptr.

TEST_CASE("AssetReferenceIndex tombstones a deleted asset that is still referenced", "[editor]")
// A->B (both exist); Update(B, false, nullopt) -> B is a tombstone
// (exists=false, inbound {A}), DanglingTargets() == {B}; its outbound edges
// were removed. Then Update(A, true, Refs({})) erases it.

TEST_CASE("AssetReferenceIndex: a never-parsed asset contributes nothing", "[editor]")
// Update(A, true, nullopt) on a fresh A -> A exists, zero outbound, and no
// inbound appears anywhere (spec s3.2's "contributes nothing" sentence).
```

(`Refs(...)` = a tiny local helper returning
`std::optional<std::vector<Arcane::AssetRef>>`.)

- [ ] **Step 2: premake + build + run to verify failure** (link error → then
assert failures as the impl grows). `ArcaneTests.exe "[editor]" --rng-seed time`
from the exe dir.

- [ ] **Step 3: Implement** `Update` exactly in this order (the discipline IS
the order):

1. Fetch/create the node for `id`; set `node.exists = exists`.
2. If `!exists`: `RemoveOutboundEdges(id)`, clear `outbound`, and erase the node
   itself if `inbound` is empty; return.
3. If `!refs`: return (last-known-good).
4. `RemoveOutboundEdges(id)` — for each old outbound target: remove `id` from
   its `inbound` (binary search, it's sorted); if that target is
   `!exists && inbound.empty()`, erase it (tombstone GC).
5. `node.outbound = *refs;` then for each ref: fetch/create the target node
   (created nodes start `exists = false` — a target not yet walked is
   indistinguishable from a dangling one until its own `Update` arrives, which
   the full build always delivers for real assets), and insert `id` into its
   `inbound` sorted-unique.

`RemoveOutboundEdges` must iterate a COPY of / the pre-clear `outbound` — step 5
overwrites it.

- [ ] **Step 4: Run `[editor]` green; full `~[gpu]` Debug run green.**

- [ ] **Step 5: Commit** —
`feat(editor): AssetReferenceIndex -- inverted refs, inbound counts, tombstones`

---

### Task 4: Editor — model integration: the index feeds `unused`, digest goes live

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetPanelModel.hpp` (`AssetPanelEntry` gains `bool unused`, `HealthCounts` gains `int unused`, model gains the index member + accessors)
- Modify: `ArcaneEditor/src/Panels/AssetPanelModel.cpp` (`RebuildIfDirty` feeds the index; `Health()` tallies `unused`; new `UnusedGuids()`)
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp` (`DrawBottomBar` digest `:410-430`: the em-dash becomes the live count)
- Test: `ArcaneTests/src/AssetPanelModelTest.cpp`

**Interfaces:**
- Consumes: Task 3's `AssetReferenceIndex` API; the existing per-guid `refsFor`
  asks `RebuildIfDirty` already makes for every rebuilt entry (proven by the
  Plan-1 call-count test at `AssetPanelModelTest.cpp:441-495` — do NOT add a
  second ask).
- Produces (Tasks 7/8 call exactly these):
  - `const AssetReferenceIndex& RefIndex() const;` on `AssetPanelModel`
  - `std::vector<Arcane::Guid> UnusedGuids() const;` — unused entries, sorted
    by entry name (stable UI order)
  - `HealthCounts { int total, cooked, queued, refused, unused; }`
  - `AssetPanelEntry::unused`
  - the digest chip rendering `N unused` live

- [ ] **Step 1: Write the failing tests** (the `FakeProviders` + real-registry
pattern, `AssetPanelModelTest.cpp:66-100`; the `Health` case at `:542-584` is
the template):

```cpp
TEST_CASE("AssetPanelModel unused: zero-inbound eligible kinds only (spec s9.1)", "[editor]")
{
    // Fixture: texture A referenced by sprite S (DerivesFrom edge in
    // fake.refsByGuid[S]); orphan texture B (no inbound); a .json data file
    // (exempt kind); a .arcscene (root, exempt).
    // -> B.unused == true; A/S/data/scene all false; health.unused == 1
    //    (S itself: zero inbound -> ALSO unused; assert it and count 2 --
    //     the cascade rule, Ruling 7: the dead minted sprite flags first.)
}

TEST_CASE("AssetPanelModel unused updates incrementally through MarkDirty", "[editor]")
{
    // mat M refs T1 -> T1 not unused, T2 unused. Re-point fake.refsByGuid[M]
    // to T2, MarkDirty(M), rebuild -> flags swap. Proves remove-before-readd
    // flows through the model seam.
}

TEST_CASE("AssetPanelModel keeps last-known-good inbound counts on a nullopt refs answer", "[editor]")
{
    // refsFor returns nullopt for M on the second ask (FakeProviders gains a
    // per-guid "answer nullopt" switch) -> counts unchanged from the first build.
}

TEST_CASE("AssetPanelModel prunes a deleted asset into the index tombstone path", "[editor]")
{
    // Delete M's file, ScanContent again, MarkAllDirty, rebuild:
    // M's entry gone; T (previously referenced only by M) now unused;
    // model.RefIndex().DanglingTargets() does NOT contain M (nothing refs it).
    // Variant: delete T's file instead -> RefIndex().DanglingTargets()
    // contains T (M still points at it).
}

TEST_CASE("AssetPanelModel HealthCounts.unused feeds the digest numbers", "[editor]")
// health.unused matches the flagged-entry count across a mixed fixture.
```

NOTE: check how the existing prune test rescans (the prune loop is
`AssetPanelModel.cpp:146-160`) and mirror its mechanics.

- [ ] **Step 2: Run to verify failure** (`[editor]` from the exe dir).

- [ ] **Step 3: Implement.**

In `RebuildIfDirty`:
- Full pass (`m_allDirty`): `m_refIndex.Clear()` first, then for every entry
  built, `m_refIndex.Update(guid, true, refs)` with the SAME `refs` optional
  the entry build just fetched (hold it in a local before deriving fold/sliced).
- Per-guid pass: `m_refIndex.Update(guid, true, refs)` for each dirty guid;
  in the prune loop (`:146-160`), `m_refIndex.Update(guid, false, std::nullopt)`
  for each pruned entry (folded children included — they are entries).
- After entries + index settle, one pass sets
  `e.unused = IsUnusedEligible(e.kind) && m_refIndex.InboundCount(e.guid) == 0;`
  where `IsUnusedEligible(k)` = `k == Texture || k == Material || k == Sprite || k == Mesh`
  (a small named helper beside `CookStateOf`, spec §9.1's list verbatim).
  This pass runs over ALL entries even on a per-guid rebuild — inbound counts
  of untouched entries change when a dirtied one re-points (the flag is cheap;
  the parse is what invalidation protects).
- `Health()` (`:557-572`): `if (e.unused) ++h.unused;`
- `UnusedGuids()`: collect flagged entries, sort by `name`, return guids.

In `DrawBottomBar` (`AssetsPanel.cpp:419-420`): replace the em-dash segment with
`" · %d cooking · %d unused"` fed by `health.queued, health.unused`, and delete
the now-false half of the `:363-368` doc comment (the digest is live as of this
plan). The click-through arrives with Task 8, not here.

- [ ] **Step 4: Run `[editor]` green, then full `~[gpu]` Debug.** The Plan-1
call-count tests must still pass unchanged — if `refsCalls` counts moved, a
second ask snuck in; fix the implementation, not the test.

- [ ] **Step 5: Commit** —
`feat(editor): reference index wired through AssetPanelModel -- unused count live in the digest`

---

### Task 5: Editor — `AssetActivityLog` + event-seam wiring

**Files:**
- Create: `ArcaneEditor/src/Panels/AssetActivityLog.hpp`
- Create: `ArcaneEditor/src/Panels/AssetActivityLog.cpp`
- Modify: `premake5.lua` (ArcaneTests `files{}`: both new TUs), re-generate
- Modify: `ArcaneEditor/src/App/EditorApp.hpp` (member `Arcane::Editor::AssetActivityLog m_assetActivity;` near `m_assetModel` `:1195`; doc: main-thread-only, the `CookDiagRow` `:1368-1379` precedent)
- Modify: `ArcaneEditor/src/App/EditorAppProject.cpp` (push sites below)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (create-dispatcher push)
- Modify: `ArcaneEditor/src/App/EditorApp.cpp` (`:1233-1235` project-switch `Clear()`)
- Test: `ArcaneTests/src/AssetActivityLogTest.cpp` (new)

**Interfaces:**
- Produces (Task 8 reads exactly this):

```cpp
namespace Arcane::Editor
{
    enum class AssetActivityKind : std::uint8_t
    { SourceChanged, Cooked, CookRefused, Created, Deleted };
    // Deleted: vocabulary only -- the editor has no delete flow and the
    // registry no Remove API (Ruling 11). Recorded, not invented.

    struct AssetActivityEntry
    {
        std::chrono::steady_clock::time_point when;
        Arcane::Guid      guid;
        std::string       name;    // snapshot at push time; the asset may vanish
        AssetActivityKind kind = AssetActivityKind::SourceChanged;
        std::string       detail;  // refusal reason etc.; may be empty
    };

    // Session-only ring (~100, spec s9.2). Main-thread only. No persistence.
    class AssetActivityLog
    {
    public:
        static constexpr std::size_t kCapacity = 100;
        void Push(AssetActivityEntry e);   // caller stamps `when` (tests need
                                           // control over time; sites use now())
        void ForEachNewestFirst(const std::function<void(const AssetActivityEntry&)>& fn) const;
        std::size_t Size() const;
        void Clear();
    private:
        std::vector<AssetActivityEntry> m_ring;  // grows to kCapacity, then wraps
        std::size_t m_next = 0;
    };
}
```

- [ ] **Step 1: Write the failing tests** (pure, tag `"[editor]"`):
capacity (push 150 → `Size()==100`, `ForEachNewestFirst` yields the last 150th
first and exactly 100 total, the first 50 gone); ordering (three pushes come
back newest-first); `Clear()` empties.

- [ ] **Step 2: premake + build + run to verify failure.**

- [ ] **Step 3: Implement the ring** (plain modular index; `ForEachNewestFirst`
walks backward from `m_next-1`), **then wire the push sites** — each is an
existing `MarkDirty`/`MarkAllDirty` seam; add the push beside the mark, never
replacing it:

| Site | Kind | name / detail |
|---|---|---|
| `EditorAppProject.cpp:474` (external `.arcmat` edit) | `SourceChanged` | `e.name` |
| `EditorAppProject.cpp:562` (texture/`.meta` mtime) | `SourceChanged` | `e.name` |
| `EditorAppProject.cpp:427-431` (drop discovery) | `Created` | capture the discarded `RegisterCreatedAsset(dropped)` return into a local `const std::optional<Arcane::Guid> droppedId = ...;` — guid + `dropped.filename().string()` |
| `EditorAppProject.cpp:640` (cook success loop) | `Cooked` | name via registry resolve or the entry's name if in hand |
| `EditorAppProject.cpp:690` (cook failure loop) | `CookRefused` | detail = the failure `reason` |
| `EditorAppProject.cpp:729` (`OnArtifactRefused`) | `CookRefused` | detail = the refusal `kind` string |
| `EditorAppProject.cpp:133` (`onAssetSaved`) | `SourceChanged` | material name (resolve from registry) |
| `EditorAppFrame.cpp:2599-2606` (`ConsumeCreateResult` tail) | `Created` | created asset's name |
| `EditorAppScene.cpp:295-296` (`DoSaveScene` new-file branch) | `Created` | scene filename |
| `EditorAppProject.cpp:2038` (`PollDiagnosticReports`) | `Created` | report filename |

Every push stamps `when = std::chrono::steady_clock::now()` at the site.
Name lookups: where only a guid is in hand, resolve via
`project->Registry().Resolve(guid)` and take the filename; empty name is
acceptable (the feed row falls back to the guid string, Task 8).
Project switch (`EditorApp.cpp:1233-1235` block): `m_assetActivity.Clear();`.

- [ ] **Step 4: Build; `[editor]` green; boot the editor once
(`ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 60 --report <tmp>\t5-boot.json`)
to prove no push site crashes on a real project.** Full `~[gpu]` Debug run.

- [ ] **Step 5: Commit** —
`feat(editor): AssetActivityLog ring wired to the asset event seams`

---

### Task 6: Editor — widgets: `StatTile`, `MeterBar`, `CardFrame`, `TimelineFeed`

**Files:**
- Modify: `ArcaneEditor/src/Widgets/EditorWidgets.hpp` (declarations beside the Plan-1 asset widgets, `:156-233`)
- Modify: `ArcaneEditor/src/Widgets/EditorWidgets.cpp` (implementations; reuse `kPillAmberBorder` `:296` for the attention frame — same TU, no new token; record that reuse in its comment)

**Interfaces:**
- Consumes: `Theme::` tokens (`kAmber :111`, `kSeparator :93`, `kChrome :58`,
  `kWell :71`, `kPanelRaised :65`, `kTextDim :91`, `kGrab :101`),
  `GetEditorFonts()` (`EditorFonts.hpp:24`), the house widget idioms
  (SkipItems guard → `ImDrawList` paint → one real item reserves layout →
  `PushFont` for sizes — `AssetPill` `:578-606` is the model).
- Produces (Tasks 7/8 call exactly these):

```cpp
// s11.1 Plan-2 rows. All draw at the cursor and reserve their own layout space.

// Bordered card: 24px PushFont number, 13px label beneath, optional leading
// Lucide icon. variant: 0 = neutral; 1 = amber ICON (number stays text-toned).
void StatTile(const char* id, const char* number, const char* label,
              const char* iconUtf8, int variant, const ImVec2& size);

struct MeterSegment { const char* label; int count; ImU32 color; };
// Stacked horizontal bar (10-12px tall) with 2px surface gaps between
// segments, then one swatch+label+count legend row beneath (direct labels).
// Zero-count segments draw no bar slice but keep their legend entry.
void MeterBar(const char* id, const MeterSegment* segments, int count, float width);

// Bordered card region; variant 1 = the muted-amber acting-on frame (#7a5a20).
// width <= 0 -> content-region width. Height is content-driven.
bool BeginCardFrame(const char* id, int variant = 0, float width = 0.0f);
void EndCardFrame();

struct TimelineEntry { const char* age; const char* title; const char* detail; };
// Vertical line + 7px dots; per entry: dim age, normal title, dim detail line.
void TimelineFeed(const char* id, const TimelineEntry* entries, int count);
```

- [ ] **Step 1: Implement all four in one pass** (no headless tests — like the
Plan-1 widgets, `EditorWidgets.cpp` is a link-only TU in ArcaneTests; behavior
is proven by the Task 8 render comparison and the desk pass).
`BeginCardFrame`/`EndCardFrame`: use `ImDrawListSplitter` (2 channels — content
on 1, then background fill + 1px border on 0 sized from the measured group
rect) so dynamic-height cards get a background without a pre-pass; inner
padding 8px; square corners. `StatTile`: `PushFont(GetEditorFonts().interRegular, 24.0f)`
for the number, `13.0f` for the label, icon tinted `Theme::kAmber` only when
`variant == 1`. `TimelineFeed`: dots centered on a 1px `kSeparator` vertical
line, 7px diameter.

- [ ] **Step 2: Build all three configs compile-clean (0 warnings).** Run
`ArcaneTests.exe "[editor]"` (link proof).

- [ ] **Step 3: Commit** —
`feat(editor): Status-lens widgets -- StatTile, MeterBar, CardFrame, TimelineFeed`

---

### Task 7: Editor — Status lens I: enable the lens; tiles, meter, attention cards; Recook/Problems seams

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.hpp` (`AssetsPanelServices` `:130-133` gains `cookDetailFor` + `activity`; `AssetsPanelActions` `:109-122` gains `recook` + `showProblems`)
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp` (`kLensEnabledMask` `:58` → `0b101u`; body dispatch `:1722-1731`; new `DrawStatusLens` — file-local, beside `DrawBrowseLens` `:1617`)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (services adapter `:2050-2051`; `ConsumeBrowserActions` `:2300-2384` consumes the two new actions)
- Test: none headless (panel draw code is not compiled into ArcaneTests — the Plan-1 precedent; the model/index/log tests carry the logic)

**Interfaces:**
- Consumes: `HealthCounts` incl. `unused` (Task 4), `model.RefIndex()`,
  `StatTile`/`MeterBar`/`BeginCardFrame` (Task 6), `DrawAssetPeekTooltip`
  (`AssetsPanel.cpp:439-487`), `model.Select` + `selectionStamp`,
  `services.resolveAssetThumb`, `m_cookDiagnostics` via the new service.
- Produces:

```cpp
// AssetsPanelServices additions:
std::function<std::optional<std::string>(const Arcane::Guid&)> cookDetailFor;
const AssetActivityLog* activity = nullptr;   // Task 8 draws it; wire it now

// AssetsPanelActions additions ("panel reports, app performs"):
Arcane::Guid recook;          // Recook button on a refused card
bool showProblems = false;    // Problems button
```

- [ ] **Step 1: Enable + dispatch.** `kLensEnabledMask` → `0b101u` (update the
`:56-57` comment: Graph stays disabled until Plan 3). Body dispatch gains
`else if (state.lens == AssetLens::Status) DrawStatusLens(state, model, docs, services, actions);`
with the `:1729` disabled-text branch left for Graph only.

- [ ] **Step 2: `DrawStatusLens` part 1** — one scrollable child
(`##statusbody`), sections top-down per the OptionE board:

- **Tiles row**: four `StatTile`s side by side (equal widths from the content
  region): `assets` = `health.total`, `cook refused` = `health.refused`
  (variant 1, `ICON_LC_TRIANGLE_ALERT`), `awaiting cook` = `health.queued`,
  `unreferenced` = `health.unused`.
- **Cook pipeline**: section label, then
  `MeterBar` with segments cooked/queued/refused — colors:
  cooked `GetColorU32(Theme::kGrab)`, queued `GetColorU32(Theme::kTextDim)`,
  refused `GetColorU32(Theme::kAmber)` (grays + amber, icon+label carry meaning,
  never color alone).
- **Needs attention**: labeled section. For every entry with
  `cook == CookState::Refused` (iterate `model.Entries()`, sort by name):
  `BeginCardFrame(guid-id, /*variant*/1)` → row: 18px thumb
  (`services.resolveAssetThumb`), `fileName`, kind pill; second line dim:
  `services.cookDetailFor(guid)` (fallback `"cook refused"`); buttons on one
  row: `Recook` → `actions.recook = guid;`, `Problems` →
  `actions.showProblems = true;`. Card body click (not the buttons) →
  `model.Select(guid)`; hovered card → `DrawAssetPeekTooltip`. Selected card
  gets a 2px `Theme::kSelection` border (interaction contract: Status
  highlights its cards).
  For every `Queued` entry: neutral `BeginCardFrame`, name row, dim
  `"arccook running…"` line, and a 4px progress strip filled
  `health.cooked / float(health.cooked + health.queued)` (Ruling 12) in
  `kGrab` over a `kWell` track.
  Zero refused AND zero queued → one dim line `nothing needs attention`
  (empty state; flagged as a desk item, the board only shows the populated
  form).

- [ ] **Step 3: Wire the app side.** Services adapter (`EditorAppFrame.cpp:2050-2051`):

```cpp
panelServices.cookDetailFor = [this](const Arcane::Guid& g) -> std::optional<std::string>
{
    const auto it = m_cookDiagnostics.find(g);
    if (it == m_cookDiagnostics.end() || !it->second.permanent)
        return std::nullopt;
    return it->second.diagnostic.detail.empty() ? it->second.diagnostic.message
                                                : it->second.diagnostic.detail;
};
panelServices.activity = &m_assetActivity;
```

`ConsumeBrowserActions` additions:

```cpp
if (a.recook.IsValid())
{
    if (m_runtime) m_runtime->AssetsFacade().InvalidateArtifact(a.recook);
    m_cookDiagnostics.erase(a.recook);      // Refused -> Queued honestly (Ruling 8)
    PublishCookDiagnostics();               // the Problems row clears too
    if (m_cookQueue) m_cookQueue->NoteChanged();
    m_assetModel.MarkDirty(a.recook);
    m_assetActivity.Push({ std::chrono::steady_clock::now(), a.recook,
                           NameOf(a.recook), AssetActivityKind::SourceChanged,
                           "recook requested" });
}
if (a.showProblems)
{
    m_panelVis.visible[static_cast<std::size_t>(Arcane::Editor::PanelId::Problems)] = true;
    Arcane::Editor::SelectDockTab("Problems");   // the Outliner precedent, :2167-2173
}
```

(`NameOf` = the same registry-resolve-filename helper Task 5 used; hoist it if
it was written inline.) NOTE: verify the exact `m_panelVis.visible[...]`
spelling against `EditorAppFrame.cpp:2167-2173` and `Diagnostic`'s actual
member names (`detail`/`message`) at `EditorApp.hpp:1363-1367`'s usage sites.

- [ ] **Step 4: Build; run `[editor]`; boot the editor headless
(`--frames 60 --report`) — clean boot, no assert.** Capture a Status-lens
screenshot is not yet meaningful (feed/unreferenced land in Task 8) — the
structural render comparison happens there.

- [ ] **Step 5: Commit** —
`feat(editor): Status lens -- tiles, cook meter, attention cards; Recook + Problems seams`

---

### Task 8: Editor — Status lens II: Unreferenced + Reveal, Activity feed, Scenes rollup, digest click-through

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp` (`DrawStatusLens` grows the remaining sections; `DrawBottomBar` `:369-433` gains the lens-aware context line + the digest click target)
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.hpp` (only if `DrawBottomBar`'s signature needs `state` — it does: pass `const AssetsPanelState&` alongside the model)
- Test: none headless (same rationale as Task 7); `UnusedGuids` ordering already pinned in Task 4

**Interfaces:**
- Consumes: `model.UnusedGuids()`, `model.RefIndex()`, `services.activity`,
  `TimelineFeed`/`CardFrame` (Task 6), `AssetPill`, boot-scene knowledge the
  Browse rows already use for their `boot` pill (reuse the same source — find
  it in `DrawAssetRow`'s pill logic, do not invent a second).
- Produces: the complete Status lens; the digest chip switches any lens to
  Status on click.

- [ ] **Step 1: Unreferenced card.** Labeled `CardFrame`; inside, an inset well
(`Theme::kWell` fill) holding one row per `model.UnusedGuids()` (18px thumb +
`fileName` chip-style + a small `Reveal` button per row); caption beneath, dim:
`nothing points at these`. Empty state: dim `everything is referenced`.
`Reveal` (Ruling 10):

```cpp
model.SetSearch("");  state.search[0] = '\0';
model.SetKindFilter(-1);  state.railKind = -1;   // match the rail's "All" spelling
state.lens = AssetLens::Browse;
model.Select(guid);
```

NOTE: verify the exact clear-filters spellings against `AssetsPanelState`
(`AssetsPanel.hpp:57-98`) and the rail's All handling — the guarantee is "the
revealed row is visible", and the scroll-once machinery (`:1100-1114`) does the
rest.

- [ ] **Step 2: Activity feed.** Section label + `TimelineFeed` from
`services.activity->ForEachNewestFirst` (collect into a frame-local vector of
`TimelineEntry`; the strings need frame-lifetime storage — build
`std::string`s in a local vector first, then pointer views). Age formatting:
`< 60s` → `just now`, `< 60 min` → `N min ago`, else `N h ago`. Title per kind:
`Cooked` → `cooked`; `CookRefused` → `cook refused`; `Created` → `created`;
`Deleted` → `deleted`; `SourceChanged` → `source changed → queued` when the
model says the asset's kind cooks (`Texture`/`Sprite` — the `CookStateOf`
rule), else `source changed`. Detail line: `name — detail` (name falls back to
the guid string when empty). Hovered row → `DrawAssetPeekTooltip`; click →
`model.Select`. Empty state: dim `no activity yet`.

- [ ] **Step 3: Scenes rollup.** Section label `Scenes`, tucked under the feed
(the board's placement). One `CardFrame` per `kind == Scene` entry (sorted by
name): name + `boot` pill when it is the boot scene (reuse Browse's source of
that fact); count line from the index —
`n = distinct outbound targets of RefIndex().Find(guid)`; health suffix:
all targets `Cooked` → `N assets · all cooked`, else
`N assets · M need attention` (M = targets Refused or Queued, resolved through
`model.Find`). A `Focus in Graph` button drawn disabled
(`ImGui::BeginDisabled`/`TextDisabled` styling — Plan 3 enables it).

- [ ] **Step 4: Bottom bar.** `DrawBottomBar(state, model)`:
- Left context on Status: `N assets · M need attention`
  (M = `health.refused + health.queued`); Browse keeps its existing forms
  verbatim (`:400-408`).
- The digest chip becomes a click target: after drawing the two text segments,
  lay an `ImGui::InvisibleButton` over their rect (or draw them inside a
  `Selectable`-free group + `IsItemClicked` on a wrapping item) — clicked →
  `state.lens = AssetLens::Status` (spec §5's click-through; a no-op when
  already there). Keep the hairline-divider hand-painting rule (`:383-388`) —
  no layout row consumed.

- [ ] **Step 5: Render comparison (structural, never pixel).** Build; launch
`ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 120 --screenshot <tmp>\plan2-status.png`
— NOTE: the panel opens on Browse; to capture Status either drive the lens via
the panel-state default for one run or capture at the desk instead — if no
clean headless route exists, record that and lean on the desk side-by-side
(the Plan-1 treeview follow-ups used editor captures from the workspace's
compare rig, `.superpowers/sdd/2026-09-06-asset-manager-plan1/compare/`).
Compare against `renders/OptionE-Status-FINAL.png` structurally: section order
(tiles → meter → attention → unreferenced | activity → scenes), tile count 4,
meter gap 2px, dots 7px, attention frame `#7a5a20`, amber only on refused
icon/segments, §11.2 values throughout.

- [ ] **Step 6: Commit** —
`feat(editor): Status lens complete -- unreferenced+Reveal, activity feed, scenes rollup, digest click-through`

---

### Task 9: Gate, re-bless, baselines, spec addendum, desk checklist

- [ ] **Step 1:** Build Debug + Release + Dist (0 warnings each); run the FULL
suite per config (`~[gpu]` for baseline comparison AND one unfiltered Debug
run); record fresh assertion counts — derive, never recall (run the command,
paste from its own final line, capture seeds).
- [ ] **Step 2:** Re-derive `scripts/automation-baselines.json`. The committed
figures are stale TWICE over: they were measured at `2f5dc391` and the
2026-09-07 follow-up waves added ~20 cases un-attributed; Plan 2 adds more. The
note must attribute BOTH rises separately (per-suite case names, and re-derive
the `[gpu]`-invisibility argument: unfiltered minus `~[gpu]` counts).
- [ ] **Step 3:** Golden gate (`scripts\golden-gate.ps1`, Debug): the editor-ui
lane diffs against a Browse-only toolbar (Status button was disabled-dim; it is
now enabled, and the digest text changed) → a re-bless is EXPECTED. Read the
diff artifact FIRST and confirm the differing pixels sit inside the Assets
panel band; `--bless` against the SOURCE tree
(`ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 60 --settle 30 --report <path> --compare editor-ui --bless`),
restage `ReferenceProject\Verify\*` to BOTH hosts' staged trees, re-run the
gate; assert on `gatePassed` + per-lane `verdict` in
`golden-gate-summary.json`, never the exit code. Then
`golden-gate.ps1 -SelfTest` (Debug) — the gate must be observed failing.
Beware: `Content/` staging is mirrored but `Goldens/` strays recur (the Plan-1
Task-16 deferred minor) — sweep them first.
- [ ] **Step 4:** Spec addendum: append **§18 LANDED (Plan 2)** to
`docs/specs/2026-09-06-asset-manager-redesign-design.md` — scope, measured
close, deviations/rulings (including this plan's ABI-23 deviation from §14's
"one ABI bump", the pinned rulings above that survive as behavior, and
Ruling 13's dangling-data-no-UI stance). Update §17's stale "owed" lines only
by dated correction, never by rewriting history (the house §17 pattern).
- [ ] **Step 5: Desk checklist for the user** (present, don't self-certify),
written to the SDD workspace: side-by-side at 960×620 vs
`OptionE-Status-FINAL.png`; break a texture cook → tile/meter/card/digest all
move; Recook flips the card Refused→Queued live and the Problems row clears;
Problems button surfaces the pane; Reveal jumps to Browse with the row
selected+scrolled; digest chip click-through from Browse; activity feed
ordering + ages; scenes rollup counts; peek tooltip on cards and feed rows;
v3 scene (Gacha's `test.arcscene`, if mounted) still opens; re-saving
`main.arcscene` upgrades it to v4 with a manifest (then `git diff` shows the
manifest — decide at the desk whether to commit the re-save or `checkout --`
it).
- [ ] **Step 6: Commit** — `chore(editor): plan-2 gate + baselines catch-up`
(push only after the user's desk pass, per house convention).

---

## Self-review notes (kept for the executor)

- Spec §3.3 → Task 1; §3.4's demotion-to-fallback → Task 2; §9.1 → Tasks 3-4;
  §9.2 → Tasks 5-8; §11.1 Plan-2 widget rows → Task 6; §11.2 Plan-2 values →
  Tasks 6-8; §12's Plan-2 obligations (manifest round-trip + equivalence, index
  build/incremental/unused rules, last-known-good) → Tasks 1-4; §13 (digest
  never fakes a zero) → resolved by Task 4 making the count real. §10 (Graph)
  and the graph node/pin widget row are deliberately absent — Plan 3.
- The scene-manifest emitter and the structural scan share ONE identity rule
  after Task 1 — if the equivalence test (Task 2) fails, suspect a divergence
  there first.
- `Update`'s removal pass iterates the OLD outbound before overwriting — the
  index tests are built to go red if the order flips.
- The model rebuild must hold the `refsFor` answer in a local and hand the SAME
  optional to both the entry build and `m_refIndex.Update` — a second provider
  call per guid doubles engine parses and breaks the Plan-1 call-count tests.
- `MarkAllDirty` is the COMMON invalidation path (most seams widened to it in
  the Plan-1 fix wave) — `Clear()` + full re-feed on that path is deliberate
  and costs what Plan 1's full rebuild already cost.
- Font sizing uses the Plan-1 idiom `PushFont(font, sizePx)` (see
  `EditorWidgets.cpp:583`) — do not scale via `SetWindowFontScale`.
- ArcaneTests is the ONLY explicit file list: `AssetReferenceIndex.cpp`,
  `AssetActivityLog.cpp`, and both new test TUs all need `premake5.lua` entries
  + re-generation, or the suite silently never compiles them.
