# Asset Manager Plan 1 — Shell + Browse + Unified Create Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the flat Assets panel with the three-band lens shell + the Browse
lens (rail · folder-grouped table · preview), the unified Create flow, and the
interaction contract — with real thumbnails including live material previews.

**Architecture:** Two tail-appended queries on the `Assets` facade (ABI 21→22) give
the editor subkind + reference knowledge; a cached `AssetPanelModel` (invalidated
from the existing poll/cook seams, never rebuilt per frame) feeds an `AssetsPanel`
shell whose Browse lens and create dialogs are thin ImGui over the model. Old
`AssetBrowser.*` is deleted at the end after its pure helpers migrate.

**Tech Stack:** C++23, ImGui 1.92.9 (vendored), Catch2 (ArcaneTests), NRI graph
contexts for thumbnails, premake5/msbuild (VS18).

**Spec:** `docs/specs/2026-09-06-asset-manager-redesign-design.md` — read it first;
this plan implements its §3–§8, §11 fidelity rules, Plan-1 column of §2.

## Global Constraints

- Build: `Arcane.slnx` via VS18 msbuild, never a bare `.vcxproj`; `ARCANE_SDK` may
  be stale in the process env — set per-invocation if needed.
- Tests: run `ArcaneTests.exe` FROM the exe dir (`bin\Debug-windows-x86_64-md\ArcaneTests\`);
  capture the Catch2 seed banner; `~[gpu]` for baseline-comparable runs. Never
  construct a bare `Arcane::Runtime` in a test.
- ABI: this plan bumps `kPluginAbi` 21→22 once (Task 1) and appends the full delta
  to that one ledger entry as later tasks add to it (the v21-ledger precedent,
  `PluginABI.hpp:482-497`). New `Assets` virtuals go at the END (after
  `SetCookPendingProbe`, the current last slot).
- Fidelity: spec §11.2/§11.3 values are verbatim requirements (rail 180, preview
  330, rows 24, rail rows 26, thumbs 18/64/140, tooltip 210, pills 12px text /
  16px line / 1px `#333333`, amber `#ffa61a`, attention frame `#7a5a20`, kind hues
  table). All chrome colors are `EditorTheme` tokens. `MaterialSurface::Fullscreen`
  displays as the pill string `"post"`.
- Every ImGui table: `ImGuiTableFlags_NoSavedSettings`.
- **Visual keystone:** `.superpowers/design/asset-manager-mockups/README.md` — the
  FINAL board renders in `renders/` are the redline. Any task that draws UI ends
  with a structural comparison against the matching render (re-render command in
  the README; capture the editor with its own screenshot tooling; compare values
  against spec §11.2 — never a pixel diff).
- Commit after every task (message prefixes as shown); do NOT push.

---

### Task 1: Engine — `MaterialSurfaceFor` + ABI v22

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.hpp` (append after `SetCookPendingProbe`, `:264`)
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.cpp` (the facade impl class)
- Modify: `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` (`kPluginAbi` 21→22 + v22 ledger entry)
- Test: `ArcaneTests/src/AssetReferencesTest.cpp` (new)

**Interfaces:**
- Consumes: `MaterialSurface` + `MaterialSurfaceForKind` (`MaterialSource.hpp:71,77`);
  `LoadMaterialAsset`/`MaterialAssetData{kind, parent}` (`MaterialAsset.hpp:64-66`);
  the installed `AssetResolver`.
- Produces: `virtual std::optional<Arcane::MaterialSurface> MaterialSurfaceFor(const Guid& id) = 0;`
  — instance-aware (walks `parent`, bounded depth 8, cycle-safe). Later tasks
  (4, 8, 14) call exactly this name.

- [x] **Step 1: Write the failing tests** in `AssetReferencesTest.cpp` (tag
  `[assets]`, modeled on `AssetBrowserTest.cpp:57`'s temp-dir + real-files
  pattern — create facade via `Assets::Create()`, install a resolver lambda that
  maps test guids to the temp files):

```cpp
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;

namespace
{
    fs::path WriteFile(const fs::path& dir, const char* name, const std::string& text)
    {
        fs::path p = dir / name;
        std::ofstream(p) << text;
        return p;
    }
}

TEST_CASE("MaterialSurfaceFor reads the kind string; instances resolve through parent", "[assets]")
{
    const fs::path dir = fs::temp_directory_path() / "arc_matsurface_test";
    fs::create_directories(dir);
    const auto base = WriteFile(dir, "base.arcmat",
        R"({"id":"7e5a0001-0001-4001-8001-000000000001","kind":"sprite","name":"B","params":{},"snippet":"","type":"material"})");
    const auto inst = WriteFile(dir, "inst.arcmat",
        R"({"id":"7e5a0001-0001-4001-8001-000000000002","parent":"7e5a0001-0001-4001-8001-000000000001","params":{},"type":"material"})");

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path> {
        const std::string g = id.guid.ToString();
        if (g == "7e5a0001-0001-4001-8001-000000000001") return base;
        if (g == "7e5a0001-0001-4001-8001-000000000002") return inst;
        return std::nullopt;
    });

    const Arcane::Guid baseId = Arcane::Guid::FromString("7e5a0001-0001-4001-8001-000000000001");
    const Arcane::Guid instId = Arcane::Guid::FromString("7e5a0001-0001-4001-8001-000000000002");
    REQUIRE(assets->MaterialSurfaceFor(baseId) == Arcane::MaterialSurface::Sprite);
    REQUIRE(assets->MaterialSurfaceFor(instId) == Arcane::MaterialSurface::Sprite); // via parent
    REQUIRE_FALSE(assets->MaterialSurfaceFor(Arcane::Guid::Generate()).has_value()); // unresolvable
    fs::remove_all(dir);
}
```

  Also add a cycle case (two instances parenting each other → `nullopt`, no hang)
  and a non-material case (a `.png` guid → `nullopt`).
  NOTE for the executor: check `Guid`'s actual from-string spelling
  (`Guid::FromString` vs a ctor) in `Arcane/Guid.hpp` and match it; same for
  `AssetId`'s member name.

- [x] **Step 2: Add the test file to the ArcaneTests premake file list**, regenerate
  (`GenerateProjects.bat`), build Debug, run
  `ArcaneTests.exe "[assets]" --rng-seed time` from the exe dir. Expected: FAIL to
  compile (`MaterialSurfaceFor` undeclared).

- [x] **Step 3: Implement.** In `Assets.hpp`, append after `SetCookPendingProbe`
  (with a doc comment citing the v21 tail-append precedent):

```cpp
        // Asset-manager arc (ABI v22): the material SUBKIND for `id`, resolved
        // through the installed AssetResolver. Reads the .arcmat "kind" string
        // ("fullscreen"/"sprite"/"mesh"); an INSTANCE file carries no kind --
        // only "parent" -- so this walks the parent chain (bounded, cycle-safe)
        // to the base material's kind. nullopt: not a material, unreadable, or
        // an unresolvable/cyclic chain. Appended at the END of the interface
        // (the ArtifactFor/InvalidateArtifact precedent).
        virtual std::optional<MaterialSurface> MaterialSurfaceFor(const Guid& id) = 0;
```

  (`#include <Arcane/Material/MaterialSource.hpp>` for `MaterialSurface`.)
  In `Assets.cpp`'s impl class, implement via the existing JSON loader path:

```cpp
std::optional<MaterialSurface> MaterialSurfaceFor(const Guid& id) override
{
    Guid current = id;
    for (int depth = 0; depth < 8; ++depth)
    {
        auto json = GetJson(AssetId::FromGuid(current));
        if (!json) return std::nullopt;
        if (auto it = json->find("kind"); it != json->end() && it->is_string())
            return MaterialSurfaceForKind(it->get<std::string>());
        if (auto it = json->find("parent"); it != json->end() && it->is_string())
        {
            const Guid parent = Guid::FromString(it->get<std::string>());
            if (!parent.IsValid() || parent == current) return std::nullopt;
            current = parent;
            continue;
        }
        // A material file with neither kind nor parent: LoadMaterialAsset
        // defaults such a file to "fullscreen" (MaterialAsset.cpp:268-270),
        // but only if it IS a material -- gate on the "type" discriminator.
        if (auto it = json->find("type"); it != json->end() && *it == "material")
            return MaterialSurface::Fullscreen;
        return std::nullopt;
    }
    return std::nullopt; // chain too deep / cyclic
}
```

  In `PluginABI.hpp`: bump `kPluginAbi` to 22 and write the v22 ledger entry in the
  established prose style: date, "asset-manager arc", `MaterialSurfaceFor` appended
  at the tail (no existing slot moves; the bump is the restamp discipline), noting
  the entry will be EXTENDED by `ListAssetReferences` (Task 2) under this same
  number.

- [x] **Step 4: Build + run** `ArcaneTests.exe "[assets]"` from the exe dir.
  Expected: PASS (including the pre-existing `[assets]` cases — none may regress).

- [x] **Step 5: Commit** — `feat(assets): MaterialSurfaceFor facade query (ABI v22)`

---

### Task 2: Engine — `ListAssetReferences` for sprite / material / mesh

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.hpp`, `Assets.cpp`
- Modify: `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` (extend the v22 entry)
- Test: `ArcaneTests/src/AssetReferencesTest.cpp`

**Interfaces:**
- Produces (verbatim; Tasks 3, 4, 8 consume these spellings):

```cpp
    enum class AssetRefKind : std::uint8_t { References, DerivesFrom };
    struct AssetRef
    {
        Guid target;
        AssetRefKind kind = AssetRefKind::References;
    };
    // On the Assets interface, appended after MaterialSurfaceFor:
    virtual std::optional<std::vector<AssetRef>> ListAssetReferences(const Guid& id) = 0;
```

- [x] **Step 1: Write the failing tests** (same file/pattern as Task 1). Fixtures
  and expectations:
  - `plain.arcsprite` (`{"id":..., "texture":"<texGuid>", "ppu":64.0, ...}`, no
    slicing keys) → exactly one ref: `{texGuid, DerivesFrom}`.
  - a sliced sprite (add the sub-rect key) → `{texGuid, References}`.
    **Executor verification step:** read `SpriteAsset.hpp`/`SaveSpriteAsset` to
    learn the actual slicing/sub-rect field name(s) before writing this fixture;
    encode them in a file-local `IsSlicedSpriteJson(const nlohmann::json&)`.
  - `mat.arcmat` with two texture params + one float param → refs = both texture
    guids, `References`.
  - `inst.arcmat` with `"parent"` → `{parentGuid, DerivesFrom}` plus its own
    texture-param overrides as `References`.
  - `mesh.arcmesh` with `"material":"<guid>"` → `{materialGuid, References}`;
    with nil material (`00000000-...`) → empty list.
  - a `.png` guid → empty list (leaf), NOT `nullopt`.
  - an unresolvable guid → `nullopt`.

- [x] **Step 2: Build + run.** Expected: FAIL to compile (`ListAssetReferences`
  undeclared).

- [x] **Step 3: Implement.** Extraction switch keyed by the resolved path's
  lowercase extension (reuse the file's own classification helpers if present in
  `Assets.cpp`; else a local `LowerExt`):

```cpp
std::optional<std::vector<AssetRef>> ListAssetReferences(const Guid& id) override
{
    // Resolve to learn the FORMAT; leaf/opaque formats return empty (never null).
    auto resolved = ResolvePath(id);            // the same resolver step GetJson uses
    if (!resolved) return std::nullopt;
    const std::string ext = LowerExt(*resolved);

    static constexpr std::string_view kLeaf[] = { ".png", ".jpg", ".jpeg", ".tga",
        ".bmp", ".hdr", ".wav", ".ogg", ".mp3", ".flac", ".ttf", ".otf" };
    static constexpr std::string_view kOpaque[] = { ".json", ".arcdiag" };
    for (auto e : kLeaf)   if (ext == e) return std::vector<AssetRef>{};
    for (auto e : kOpaque) if (ext == e) return std::vector<AssetRef>{};

    auto json = GetJson(AssetId::FromGuid(id));
    if (!json) return std::nullopt;
    std::vector<AssetRef> out;
    auto addGuid = [&](const nlohmann::json& v, AssetRefKind kind) {
        if (!v.is_string()) return;
        const Guid g = Guid::FromString(v.get<std::string>());
        if (g.IsValid()) out.push_back({ g, kind });
    };

    if (ext == ".arcsprite")
    {
        if (auto it = json->find("texture"); it != json->end())
            addGuid(*it, IsSlicedSpriteJson(*json) ? AssetRefKind::References
                                                   : AssetRefKind::DerivesFrom);
        return out;
    }
    if (ext == ".arcmat")
    {
        if (auto it = json->find("parent"); it != json->end())
            addGuid(*it, AssetRefKind::DerivesFrom);
        if (auto params = json->find("params"); params != json->end() && params->is_object())
            for (const auto& [name, p] : params->items())
                if (p.is_object() && p.value("type", "") == "texture")
                    addGuid(p["value"], AssetRefKind::References);
        return out;
    }
    if (ext == ".arcmesh")
    {
        if (auto it = json->find("material"); it != json->end())
            addGuid(*it, AssetRefKind::References);
        return out;
    }
    if (ext == ".arcscene")
        return ScanSceneReferences(*json);   // Task 3; stub returning {} until then
    return std::vector<AssetRef>{};          // unknown format: documented empty (Task 3 test pins the table)
}
```

  Extend the v22 ledger entry (`AssetRef`/`AssetRefKind` are new BY-VALUE types in
  the header + one appended virtual; tail-append, no slot moves).

- [x] **Step 4: Build + run** `"[assets]"`. Expected: PASS.

- [x] **Step 5: Commit** — `feat(assets): ListAssetReferences (sprite/material/mesh extractors)`

---

### Task 3: Engine — scene structural scan + extension coverage test

**Files:**
- Modify: `ArcaneClient/src/Arcane/Assets/Assets.cpp` (implement `ScanSceneReferences`)
- Test: `ArcaneTests/src/AssetReferencesTest.cpp`

**Interfaces:**
- Consumes: the `{hi, lo}` guid encoding scenes use (verified in
  `main.arcscene`: `"material": {"hi": <u64>, "lo": <u64>}`, nil = both zero) and
  the identity-key rule (exactly `"id"`/`"guid"`, case-insensitive — mirror of
  `IsIdentityGuidFieldName`, `AssetBrowser.hpp:161`).
- Produces: scene refs from `ListAssetReferences`, `References` kind, deduplicated,
  filtered to guids the caller can resolve.

- [x] **Step 1: Write the failing tests.** Fixture: a minimal scene JSON with (a) a
  component guid field `"material": {"hi": H, "lo": L}` for a registered material,
  (b) an identity field `"id": {"hi":..., "lo":...}` that must NOT be reported,
  (c) a nil `{"hi":0,"lo":0}` that must NOT be reported, (d) a nonzero pair that
  resolves to nothing (must NOT be reported — the resolvability filter). Plus the
  **coverage test**: for every extension `AssetKindOf` (`AssetBrowser.hpp:59`)
  classifies, `ListAssetReferences` on a minimal file of that format returns a
  value (empty ok) — never `nullopt` for a readable file. This is the spec §3.2
  guarantee that a future format can't silently drop out.
  **Executor note:** the hi/lo→Guid reconstruction must match the JSON bridge's
  own encoding — find the serializer's guid write (SceneSerializer.hpp) and reuse
  its exact packing rather than guessing byte order.

- [x] **Step 2: Build + run.** Expected: scene case FAILS (stub returns `{}`).

- [x] **Step 3: Implement** — recursive walk of the scene JSON:

```cpp
static void ScanSceneJson(const nlohmann::json& node,
                          const std::function<bool(const Guid&)>& resolvable,
                          std::vector<AssetRef>& out)
{
    if (node.is_object())
    {
        for (const auto& [key, value] : node.items())
        {
            if (value.is_object() && value.size() == 2 &&
                value.contains("hi") && value.contains("lo") &&
                value["hi"].is_number_unsigned() && value["lo"].is_number_unsigned())
            {
                std::string lower(key);
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (lower == "id" || lower == "guid") continue;      // identity, not a ref
                const Guid g = GuidFromHiLo(value["hi"].get<std::uint64_t>(),
                                            value["lo"].get<std::uint64_t>());
                if (g.IsValid() && resolvable(g))
                    out.push_back({ g, AssetRefKind::References });
                continue;
            }
            ScanSceneJson(value, resolvable, out);
        }
    }
    else if (node.is_array())
        for (const auto& v : node) ScanSceneJson(v, resolvable, out);
}
```

  `resolvable` = "the installed AssetResolver answers for this guid". Deduplicate
  `out` by guid before returning.

- [x] **Step 4: Build + run** `"[assets]"`, then the FULL `~[gpu]` suite from the
  exe dir. Expected: PASS, no regressions.

- [x] **Step 5: Commit** — `feat(assets): scene structural reference scan + format coverage test`

---

### Task 4: Editor — `AssetPanelModel` core

**Files:**
- Create: `ArcaneEditor/src/Panels/AssetPanelModel.hpp`, `AssetPanelModel.cpp`
- Modify: `Arcane.slnx` file lists via premake (editor project) + regenerate
- Test: `ArcaneTests/src/AssetPanelModelTest.cpp` (new, `[editor]`)

**Interfaces:**
- Consumes: `AssetRegistry::All()` (`std::vector<std::pair<Guid, std::string>>`),
  `AssetKindOf`/`AssetEntry` semantics (kept — this header will absorb them in
  Task 14; until then include `Panels/AssetBrowser.hpp`).
- Produces (verbatim — Tasks 5, 9, 10, 11 consume these):

```cpp
namespace Arcane::Editor
{
    enum class CookState : std::uint8_t { Cooked, Queued, Refused, Unknown };

    struct AssetPanelProviders
    {
        std::function<std::optional<Arcane::MaterialSurface>(const Arcane::Guid&)> surfaceFor;
        std::function<std::optional<std::vector<Arcane::AssetRef>>(const Arcane::Guid&)> refsFor;
        std::function<CookState(const Arcane::Guid&)> cookStateFor;
    };

    struct AssetPanelEntry
    {
        Arcane::Guid guid;
        std::string  name;        // stem
        std::string  fileName;    // stem + extension (rows show this)
        std::string  mountPath;
        std::string  folder;      // "materials/", nested "fx/glow/", root = "Content/"
        AssetKind    kind = AssetKind::Other;
        std::optional<Arcane::MaterialSurface> surface;  // materials only
        bool         isInstance = false;
        CookState    cook = CookState::Unknown;
        Arcane::Guid foldedUnder;                 // valid => render only as a child
        std::vector<Arcane::Guid> derivedChildren; // 1:1 sprites folded under me
    };

    struct AssetPanelRow
    {
        enum class Type : std::uint8_t { Group, Asset, Child };
        Type type = Type::Asset;
        std::string  groupName;   // Type::Group
        int          groupCount = 0;
        Arcane::Guid guid;        // Asset/Child
    };

    struct RailEntry { int kind = -1; std::string label; int count = 0; }; // kind -1 = All

    struct HealthCounts { int total = 0, cooked = 0, queued = 0, refused = 0; };

    class AssetPanelModel
    {
    public:
        void MarkDirty(const Arcane::Guid& id);
        void MarkAllDirty();
        // Rebuilds entries for dirty guids (all, when all-dirty) and the row list
        // when entries OR filters changed. Cheap when clean. Returns true if
        // anything rebuilt. registry == nullptr clears the model.
        bool RebuildIfDirty(const Arcane::AssetRegistry* registry,
                            const AssetPanelProviders& p);

        void SetSearch(std::string_view s);       // MatchesFilter semantics
        void SetKindFilter(int kindOrMinus1);
        void SetGroupOpen(const std::string& folder, bool open);
        void SetChildrenOpen(const Arcane::Guid& texture, bool open);

        [[nodiscard]] const std::vector<AssetPanelRow>& Rows() const;
        [[nodiscard]] const std::vector<RailEntry>&     Rail() const;
        [[nodiscard]] HealthCounts                       Health() const;
        [[nodiscard]] const AssetPanelEntry*             Find(const Arcane::Guid&) const;
        [[nodiscard]] int  ShownAssetCount() const;      // for "X of N shown"
        [[nodiscard]] bool Filtered() const;             // search or kind filter active

        Arcane::Guid   selected;                          // THE shared selection
        std::uint32_t  selectionStamp = 0;                // bump on every change
        void Select(const Arcane::Guid& g) { if (g != selected) { selected = g; ++selectionStamp; } }
        void ResetForProjectSwitch();                     // clears everything incl. selected
    };
}
```

  Semantics to implement: folder = directory portion of the mount path after
  `scheme://` (root files → `"Content/"`); groups sorted lexicographically, rows
  within a group by fileName; fold = an entry whose refs contain exactly one
  `DerivesFrom` to a texture in the registry gets `foldedUnder` set and its parent
  gains it in `derivedChildren`; child rows render under the parent only when the
  parent's `SetChildrenOpen` is true (default false) and always match search
  independently; groups with zero visible rows are dropped; the rail hides
  zero-count kinds; `Health().total` counts every registry entry (folded children
  included); rows honor group-open state.

- [x] **Step 1: Write the failing tests** — real temp dir + real files +
  `registry.ScanContent(dir, "game")` (the `AssetBrowserTest.cpp:57` pattern),
  with provider lambdas faked in-test (no engine facade). Cases: (a) grouping +
  ordering; (b) fold (a 1:1 sprite disappears as peer, appears under expanded
  texture, count pill data via `derivedChildren.size()`); (c) search filters
  children independently; (d) kind filter + `ShownAssetCount`; (e) rail counts
  hide empty kinds; (f) `MarkDirty` on one guid re-asks providers ONLY for it
  (count provider invocations in the lambda); (g) `ResetForProjectSwitch` clears
  selection; (h) `Health` counts refused/queued from the provider.

- [x] **Step 2: Regenerate, build, run** `"[editor]"`. Expected: FAIL to compile.

- [x] **Step 3: Implement** `AssetPanelModel.cpp`: entry cache
  `std::unordered_map<Guid, AssetPanelEntry>` + `dirty` set + `allDirty` flag +
  cached rows/rail rebuilt when `rowsDirty`. Keep every function out of ImGui —
  this TU compiles in the test gate.

- [x] **Step 4: Build + run** `"[editor]"`. Expected: PASS.

- [x] **Step 5: Commit** — `feat(editor): AssetPanelModel — cached, foldable asset model`

---

### Task 5: Editor — model wiring (dirty marks, providers, cook state)

**Files:**
- Modify: `ArcaneEditor/src/App/EditorApp.hpp` (member `Arcane::Editor::AssetPanelModel m_assetModel;` + provider builder decl)
- Modify: `ArcaneEditor/src/App/EditorAppProject.cpp` (`PollAssetWatch` `:208-391`, `OnCookCompleted` `:401-465`, `OnProjectOpened`, the mint functions)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (rebuild call before the panel draw, `:2030` region)
- Test: `ArcaneTests/src/AssetPanelModelTest.cpp` (cook-state mapping is pure — test the mapping helper)

**Interfaces:**
- Consumes: `EditorApp::IsCookPending(guid)` (`EditorApp.hpp:1310`),
  `m_cookDiagnostics` rows (`EditorApp.hpp:1260-1277`, `CookDiagRow{diagnostic, permanent}`),
  `m_runtime->AssetsFacade()` (Tasks 1–3 queries), `CookResult::cookedGuids`.
- Produces: `AssetPanelProviders EditorApp::MakeAssetPanelProviders();` and the
  invariant later tasks rely on: **the model is current before any panel draw**.

- [x] **Step 1: Write the failing test** for the pure cook-state mapping (new free
  function in `AssetPanelModel.hpp`):

```cpp
// permanentDiag: a permanent cook-diagnostic row exists for the guid (refusal).
// pending: EditorApp::IsCookPending answer. Textures/sprites cook; other kinds
// report Cooked unless refused (they have no cook pipeline of their own).
[[nodiscard]] CookState CookStateOf(AssetKind kind, bool permanentDiag, bool pending);
```

  Cases: refused wins over pending; texture pending → Queued; material never
  Queued; unknown-kind default Cooked.

- [x] **Step 2: Run** `"[editor]"` — FAIL (undeclared).

- [x] **Step 3: Implement + wire.**
  - `CookStateOf` in the model TU.
  - `MakeAssetPanelProviders()` in `EditorAppProject.cpp`: `surfaceFor`/`refsFor`
    call the facade; `cookStateFor` = `CookStateOf(kind, HasPermanentCookDiag(g), IsCookPending(g))`
    (add the small `HasPermanentCookDiag` reader over `m_cookDiagnostics`).
  - Dirty marks: `OnCookCompleted` — inside the existing `cookedGuids` loop
    (`EditorAppProject.cpp:401-465`) add `m_assetModel.MarkDirty(guid);`.
    `PollAssetWatch` — material mtime change (`:280-323`) and texture/meta change
    (`:353-372`) add `MarkDirty`; the drop-discovery register (`:253-275`) adds
    `MarkAllDirty()` (a new registry entry changes grouping).
    `RegisterCreatedAsset` mint sites (`CreateMaterialAt`, `MintOrReuseSpriteForTexture`,
    `MintMeshAsset`, `DoSaveScene`) — `MarkAllDirty()` after successful register.
    `OnProjectOpened` — `m_assetModel.ResetForProjectSwitch(); m_assetModel.MarkAllDirty();`.
  - Rebuild: in `DrawEditorUi` immediately before the Assets panel draw:
    `m_assetModel.RebuildIfDirty(proj ? &proj->Registry() : nullptr, m_assetPanelProviders);`
    (cache the providers struct as a member, built once per project open).

- [x] **Step 4: Build editor + tests; run** `"[editor]"`. Expected: PASS; editor
  boots ReferenceProject with no behavior change (old panel still drawing).

- [x] **Step 5: Commit** — `feat(editor): asset model wired to poll/cook/mint seams`

---

### Task 6: Editor — widgets: `AssetPill`, `SegmentedStrip`, `RowWithThumb`, `AssetPeekTooltip`

**Files:**
- Modify: `ArcaneEditor/src/Widgets/EditorWidgets.hpp`, `EditorWidgets.cpp`

**Interfaces (verbatim — Tasks 9–11 consume):**

```cpp
    // 12px bordered label (spec §11.2). variant: 0 = neutral (#333 border,
    // TextDisabled-ish 9a9a9a text), 1 = amber (border #7a5a20, text kAmber).
    void AssetPill(const char* text, int variant = 0);

    // Right-most segmented switch. items are labels; enabledMask bit i gates item
    // i; returns the clicked index or -1. Draw with collapsed shared borders,
    // square corners, active = kButtonActive.
    [[nodiscard]] int SegmentedStrip(const char* id, const char* const* items,
                                     int count, int active, unsigned enabledMask);

    // One 24px selectable asset row: 18px thumb (textureId 0 => icon fallback,
    // iconUtf8 = a Lucide glyph), name, then caller-drawn trailing content via
    // the returned scope. Returns clicked.
    struct [[nodiscard]] AssetRowResult { bool clicked = false; bool hovered = false; };
    AssetRowResult RowWithThumb(const char* id, ImTextureID thumb, const char* iconUtf8,
                                const char* name, bool selected, float indent);
```

  `AssetPeekTooltip` lives with the panel (Task 9) because it needs the model +
  thumb resolver; the widgets above are model-free. Match file conventions:
  label-first params, doc comment with the spec citation, `[[nodiscard]]` on
  predicates.

- [x] **Step 1–4:** These are ImGui draw helpers — the test gate does not compile
  them (`AssetBrowserTest.cpp:1-3` discipline); correctness is desk-verified in
  Task 16. Implement with drawlist primitives: `AssetPill` = `GetWindowDrawList()`
  rect + border (`ImGui::GetColorU32` of theme tokens) around a 12px
  `PushFont`-sized text; `SegmentedStrip` = N `ImGui::Button`s with
  `ImGuiStyleVar_ItemSpacing` (0,0), `PushStyleColor(ImGuiCol_Button, kButtonActive)`
  for the active item, `BeginDisabled` per cleared mask bit; `RowWithThumb` =
  `ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns |
  ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 24))` then same-line overdraw
  of `ImGui::Image` (18×18) or the icon glyph, then the name. Build the editor;
  it must compile clean with zero warnings.

- [x] **Step 5: Commit** — `feat(editor): asset panel widget vocabulary (pill/strip/row)`

---

### Task 7: Editor — thumbnail resolver seam

**Files:**
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp` (extend `InspectorServices`-style services — add to the services struct the panels receive)
- Modify: `ArcaneEditor/src/App/EditorApp.cpp` (wire, next to `resolveTexturePreview` at `:805-814`)

**Interfaces:**
- Consumes: `ChromeGraph()->Textures()->Resolve(guid, ColorSpace::Display)` (the
  exact `resolveTexturePreview` recipe, `EditorApp.cpp:805-814`); the model's
  refs (a sprite's thumb = its texture's thumb via the single `DerivesFrom`/
  texture ref).
- Produces: `std::function<std::uint64_t(const Arcane::Guid&)> resolveAssetThumb;`
  — returns an ImTextureID or 0 (caller falls back to the kind icon). Textures:
  direct resolve. Sprites: resolve their referenced texture. Materials: Task 8's
  harvester (until then 0). Everything else: 0.

- [x] **Step 1: Implement the lambda** in `EditorApp.cpp` (no new cache — the
  chrome `NriTextureCache` IS the cache, and `OnCookCompleted` already invalidates
  it at `EditorAppProject.cpp:443-444`):

```cpp
m_assetServices.resolveAssetThumb = [this](const Arcane::Guid& guid) -> std::uint64_t {
    Arcane::NriGraphContext* chrome = ChromeGraph();
    Arcane::NriTextureCache* cache = chrome ? chrome->Textures() : nullptr;
    if (!cache) return 0;
    const auto* e = m_assetModel.Find(guid);
    if (!e) return 0;
    Arcane::Guid tex;
    if (e->kind == Arcane::Editor::AssetKind::Texture) tex = guid;
    else if (e->kind == Arcane::Editor::AssetKind::Sprite)
        tex = FirstTextureRefOf(guid);            // small helper over refsFor
    else if (e->kind == Arcane::Editor::AssetKind::Material)
        return m_materialThumbs ? m_materialThumbs->ThumbTextureId(guid) : 0; // Task 8
    if (!tex.IsValid()) return 0;
    nri::Texture* t = cache->Resolve(tex, Arcane::NriTextureCache::ColorSpace::Display);
    return t ? (std::uint64_t)(std::intptr_t)t : 0;
};
```

- [x] **Step 2: Build; boot ReferenceProject headless smoke** (the editor exe must
  still open clean). Expected: no behavior change yet (nothing calls the seam).

- [x] **Step 3: Commit** — `feat(editor): asset thumbnail resolver seam (chrome texture cache)`

---

### Task 8: Editor — `MaterialPreviewHarvester` (live material thumbs)

**Files:**
- Create: `ArcaneEditor/src/Project/MaterialPreviewHarvester.hpp`, `.cpp`
- Modify: `ArcaneEditor/src/App/EditorApp.hpp/.cpp` (own it, pump it, invalidate it)

**Interfaces:**
- Consumes: `NriGraphContext::CreateOffscreen(hostConfig, device, 64, 64)` +
  `RenderFrameOffscreen(FrameDesc)` + `ReadCapture(w, h, rgba)` — mirror
  `ShaderEditorDocument::EnsureGraphPreviewContext` (`:1530-1585`) and the render
  recipe at `:1642-1730`, at 64px, with `vp.capture = true`; upload via the chrome
  `NriTextureCache` pixel-supply route (the toolbar-logo pattern:
  `m_graphLogoTexture`, `EditorApp.hpp:1449` — synthetic per-material Guid +
  `ColorSpace::Display`).
- Produces:

```cpp
class MaterialPreviewHarvester
{
public:
    // Queue a material for (re)harvest. Cheap, dedupes.
    void Invalidate(const Arcane::Guid& material);
    // At most ONE harvest per call (ReadCapture idles the device -- the
    // EditorApp.cpp:1374-1395 rationale; keep it to editor-idle cadence).
    void Pump(/* services: device, hostConfig, chrome cache, assets facade, resolver */);
    // 0 until harvested.
    [[nodiscard]] std::uint64_t ThumbTextureId(const Arcane::Guid& material) const;
};
```

- [x] **Step 1: Implement.** Preview content: the same quad-on-checkerboard the
  shader editor's preview renders (real shaded pixels of the actual material —
  sprite and fullscreen surfaces via the `Batcher2D` path, mesh-surface materials
  via the mesh-preview path if trivially reachable, else the same quad; note
  which in the code comment). After `ReadCapture`, hand the tight RGBA to the
  chrome cache under a per-material synthetic guid with a `PixelSupplyFn` serving
  the stored bytes; re-`Invalidate` replaces the bytes and invalidates the cache
  entry (`NriTextureCache::Invalidate` + `ImGuiNri::InvalidateUserTextureNow`
  obligations per `NriGraphContext.hpp:163-175`).
- [x] **Step 2: Wire invalidation:** material mtime change in `PollAssetWatch`
  (`:280-323`), material save (the shader editor's save path — find
  `SaveMaterialAsset` call sites in `ShaderEditorDocument.cpp`), cook completion
  for the material's textures, and **parent-chain fan-out**: when material M
  changes, also `Invalidate` every registry material whose `MaterialSurfaceFor`
  chain passes through M (walk the model's refs: instances hold a `DerivesFrom`
  ref to M).
- [x] **Step 3: Persist harvests (the UE lesson — persisted thumbs beat re-renders;
  cf. UE's in-package `FObjectThumbnail` + `EThumbnailRenderFrequency::OnAssetSave`):**
  after a successful harvest, write the 64px RGBA to
  `<project>/Saved/Thumbnails/<guid>.png` via `Arcane::WriteThumbnailPngRgba`
  (`Assets.hpp:375`); on project open, do NOT queue harvests — load existing PNGs
  via `Arcane::LoadDisplayPixels` (`Assets.hpp:348`, maxSize 64) straight into the
  chrome-cache pixel supply, and queue a harvest ONLY for materials with no PNG or
  whose `.arcmat` mtime is newer than the PNG's. Each `Invalidate` deletes the
  PNG's claim (re-harvest overwrites it). This turns N-device-idles-at-boot into
  zero for an unchanged project.
- [x] **Step 4: Pump site + ordering:** call `m_materialThumbs->Pump(...)` once per
  frame in `PumpEditorDocuments` (next to `PollAssetWatch`). The queue is a LIFO
  stack and the Browse draw pushes any *visible* un-thumbed material each frame —
  so the frame's one harvest is always something on screen (UE's pool is LIFO for
  exactly this reason, `AssetThumbnail.cpp:2104-2120`).
- [x] **Step 4: Build + boot ReferenceProject; watch the log** — three materials
  harvest within ~3 frames, no device-lost, no per-frame idle after that.
  `[gpu]`-adjacent risk: run `ArcaneTests.exe "[gpu]"` once to confirm no
  regression (offscreen contexts are test-covered).
- [x] **Step 5: Commit** — `feat(editor): live material preview thumbnails (64px harvest)`

---

### Task 9: Editor — `AssetsPanel` shell (bands, contracts, lens strip)

**Files:**
- Create: `ArcaneEditor/src/Panels/AssetsPanel.hpp`, `AssetsPanel.cpp`
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp:2030-2035` (call-site swap)

**Interfaces:**
- Consumes: model (Task 4/5), widgets (Task 6), thumb seam (Task 7),
  `PanelId::Assets` visibility flags (unchanged — same `"Assets"` Begin title, so
  `PanelRegistry.hpp:37` and imgui.ini keys are untouched).
- Produces (Tasks 10–13 extend these structs in place):

```cpp
    enum class AssetLens : std::uint8_t { Browse, Graph, Status };

    struct AssetsPanelState
    {
        AssetLens lens = AssetLens::Browse;
        char search[128] = {};
        int  railKind = -1;                 // -1 = All
        std::uint32_t seenSelectionStamp = 0; // scroll-to-selection once
    };

    struct AssetsPanelActions   // superset of today's AssetBrowserActions
    {
        Arcane::Guid createInstanceOf, createSpriteFrom, setBootScene,
                     showInExplorer, copyPath, copyGuid;
        std::filesystem::path openScene;
        // Unified create (Task 12): request the create dialog for a kind.
        // -1 = none. Values = CreateAssetKind (Task 12).
        int  requestCreateKind = -1;
        Arcane::Guid createPrefillParent;   // instance parent / sprite texture prefill
    };

    struct AssetsPanelServices
    {
        std::function<std::uint64_t(const Arcane::Guid&)> resolveAssetThumb;
    };

    AssetsPanelActions DrawAssetsPanel(AssetsPanelState& state, AssetPanelModel& model,
                                       const Arcane::Project* project, DocumentHost& docs,
                                       const AssetsPanelServices& services,
                                       bool* open = nullptr);
```

- [x] **Step 1: Implement the shell** (Browse body is a placeholder child region
  until Task 10): `ImGui::Begin("Assets", open)`; toolbar row = `+ Create`
  button (`ICON_LC_PLUS " Create"`) which opens `BeginPopup("##createmenu")` — the
  unified menu per the CreateFlow mock (`Material…`, `Material Instance…`,
  separator, `Mesh…`, `Sprite…`, `Scene…`), each item setting
  `requestCreateKind = (int)CreateAssetKind::<X>` (until Task 12 defines the
  enum, gate the menu items behind `#if` nothing — just leave the popup drawing
  disabled entries; Task 12 enables them), search
  (`InputTextWithHint`, width = remaining minus strip minus per-lens slot),
  `SegmentedStrip("##lens", {"Browse","Graph","Status"}, 3, (int)state.lens,
  0b001)` — Graph/Status disabled; bottom bar per spec §5:
  left `N assets · 1 selected` / `X of N shown` (model `Filtered()` /
  `ShownAssetCount()`), right digest = amber triangle glyph + `%d refused` in
  `kAmber` + dim `· %d cooking · — unused` from `model.Health()`.
- [x] **Step 2: Swap the call site** (`EditorAppFrame.cpp:2030-2035`): draw
  `DrawAssetsPanel(m_assetsPanel, m_assetModel, ...)`; map the action fields the
  old consumer already handles onto `ConsumeBrowserActions` equivalents (extend
  `ConsumeBrowserActions`'s signature to take the new struct; `copyGuid` copies
  `guid.ToString()` to the clipboard beside `copyPath` at `:2352`). The old
  `DrawAssetBrowserPanel` is no longer called (files still present until Task 14).
- [x] **Step 3: Build + boot** — panel shows toolbar/empty body/bottom bar; counts
  live; lens strip present with two disabled buttons.
- [x] **Step 4: Commit** — `feat(editor): AssetsPanel shell with lens strip + digest bar`

---

### Task 10: Editor — Browse lens: rail + grouped table

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp`

**Interfaces:**
- Consumes: `model.Rows()/Rail()`, `RowWithThumb`, `AssetPill`,
  `services.resolveAssetThumb`, `kAssetDragType`/`AssetDragPayload` (unchanged
  payload — existing drop targets keep working).
- Produces: the Browse body: left rail child (180px), center table child.

- [x] **Step 1: Implement the rail:** child window 180px, `kChrome`-style
  background band; rows 26px via `RowWithThumb` (icon = kind Lucide glyph, thumb
  0); trailing count right-aligned dim; hover shows the `+` mini-button for
  creatable kinds (Materials/Sprites/Meshes/Scenes) which sets
  `requestCreateKind` for that kind; click sets `state.railKind` +
  `model.SetKindFilter`.
- [x] **Step 2: Implement the table:** one `BeginTable("##assets", 1,
  RowBg | ScrollY | NoSavedSettings)`; iterate `model.Rows()` **through an
  `ImGuiListClipper`** (rows are fixed 24px, so the clipper is exact; group rows
  count as rows — the flat row vector makes this trivial). Per clipped row:
  `Type::Group` → `HeaderBand`-style chrome row with chevron (toggles
  `SetGroupOpen`), name, dim count; `Type::Asset` → `RowWithThumb` (thumb from
  seam, fallback = `KindIcon` — reuse the switch from `AssetBrowser.cpp:16-39`),
  pills (subkind string — Sprite→`"sprite"`, Mesh→`"mesh"`, Fullscreen→`"post"`;
  `"inst"` when `isInstance`; `"boot"` amber variant for the boot scene —
  compare against the project's recorded boot scene guid; `"sliced"` for
  non-folded sprites with a texture ref; derived-count pill on textures with
  collapsed children; refused rows prepend `ICON_LC_TRIANGLE_ALERT` in `kAmber`);
  expander chevron on textures with children (toggles `SetChildrenOpen`);
  `Type::Child` → indented (`indent = 20.0f`) dim-name row with `"derived"` pill.
- [x] **Step 3: Interactions on every asset/child row:** click →
  `model.Select(guid)`; double-click → resolve + `docs.OpenPath` / scene →
  `actions.openScene` (copy the routing from `AssetBrowser.cpp:162-179`
  verbatim); drag source (payload identical to `AssetBrowser.cpp:128-134`);
  context menu per spec §6 (kind-specific items copied from
  `AssetBrowser.cpp:140-160`, plus `Create ▸` submenu raising
  `requestCreateKind`, plus Copy Guid); peek tooltip: implement
  `DrawAssetPeekTooltip(model, services, guid)` file-locally —
  `ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)` → `BeginTooltip` → 64px
  image/icon + name + kind/subkind pills + dim path + dim cook line + dim guid.
  Keyboard: after the loop, if the table is focused, Up/Down move `Select`
  through visible rows, Enter opens.
- [x] **Step 4: Scroll-to-selection:** when
  `state.seenSelectionStamp != model.selectionStamp`, `SetScrollHereY` on the
  selected row's draw and update the stamp.
- [x] **Step 5: Build + desk smoke** on ReferenceProject: fold visible (uv_marker
  single row + expandable child), groups collapse, search filters, drag onto an
  inspector texture slot still works. Run full `~[gpu]` tests (no regressions).
- [x] **Step 6: Commit** — `feat(editor): Browse lens — rail + folder-grouped table`

---

### Task 11: Editor — Browse lens: preview pane

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetsPanel.cpp`

- [x] **Step 1: Implement:** right child 330px (hidden when panel width < 720px):
  140px thumb (seam, icon fallback), name (stem) + kind pill + subkind/inst
  pills, `path` row (mount path, ellipsized via `EllipsisToWidth`), `guid` row
  (dim; click copies — `ImGui::SetClipboardText`), `cook` row (state string;
  refused in `kAmber`), separator, `Derived (N)` list (each row: sprite icon +
  name; click `Select`s the child), separator, action buttons full-width 24px:
  Open (same routing as double-click), Show in Explorer, Copy Path + the
  kind-specific action (materials: `New Instance…` → `createInstanceOf`; scenes:
  `Set as Boot Scene`; textures: `Create Sprite` → `createSpriteFrom`).
- [x] **Step 2: Build + desk smoke:** selection from table updates pane; empty
  selection shows a dim "no selection" line. Commit —
  `feat(editor): Browse preview pane`

---

### Task 12: Create — `CreateAssetRequest` + shared dialog + Material/Instance refactor

**Files:**
- Create: `ArcaneEditor/src/Panels/CreateAssetDialog.hpp`, `.cpp`
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (`ConsumeMenuRequests`
  `:2210-2233` — retire the two `ShowSaveFileDialog` creation launches;
  `ConsumeMaterialDialogResults` `:588-609` — route through the new path)
- Modify: `ArcaneEditor/src/Panels/EditorPanels.cpp:225-248` (Assets ▸ Create menu
  → new entries raising requests)
- Modify: `ArcaneEditor/src/App/EditorAppProject.cpp` (`CreateMaterialAt` gains an
  explicit surface param path for all three kinds — it already takes
  `MaterialSurface`, `EditorAppProject.cpp:700`)
- Test: `ArcaneTests/src/CreateAssetDialogTest.cpp` (new, `[editor]` — the pure
  validation)

**Interfaces:**
- Produces (Task 13 extends the enum; consumers: shell toolbar, rail, context menu, Assets menu):

```cpp
    enum class CreateAssetKind : std::uint8_t
    { Material, MaterialInstance, Mesh, Sprite, Scene };

    struct CreateAssetRequest
    {
        CreateAssetKind kind;
        Arcane::Guid    prefillParent;   // instance parent / sprite texture
        int             prefillSurface = -1; // pre-picked MaterialSurface, -1 none
    };

    // PURE validation (headless-tested):
    struct CreateNameCheck { bool ok; std::string message; };
    [[nodiscard]] CreateNameCheck ValidateCreateName(
        std::string_view name, const std::filesystem::path& targetDir,
        std::string_view extension);

    // Modal state (owned by EditorApp between frames).
    struct CreateDialogState
    {
        bool open = false;
        CreateAssetRequest request{};
        char name[128] = {};
        int  folderIndex = 0;       // into the folder combo
        int  surface = 0;           // Material kind combo
        Arcane::Guid parent, texture;
        bool setAsBoot = false;
        bool pickerOpen = false;
    };

    // The modal. Returns a completed request+fields when Create was clicked.
    struct CreateAssetResult
    {
        CreateAssetKind kind;
        std::string name; std::string folder;    // relative to Content/
        int surface = 0;                          // Material: MaterialSurface value
        Arcane::Guid parent, texture; bool setAsBoot = false;
    };
    std::optional<CreateAssetResult> DrawCreateAssetDialog(
        CreateDialogState& st, const AssetPanelModel& model,
        const Arcane::Project& project);
```

- [x] **Step 1: Failing tests** for `ValidateCreateName` (empty, illegal chars,
  path separators, duplicate-in-dir via a temp dir, valid) — `[editor]`.
  Validation rules, in UE's deliberate cheap→expensive order
  (`AssetViewUtils.cpp:1420-1509` precedent): (1) character deny-set
  `\ / : * ? " < > |` plus leading/trailing dots and spaces; (2) length (stem +
  extension + target dir must stay under 240 chars absolute); (3) uniqueness in
  the target directory — with a DISTINCT message ("a <kind> named X already
  exists here") from the illegal-char message, so the fix is obvious. Return
  `{ok, message}`; the dialog shows `message` dim under the Name well.
- [x] **Step 2: Run** — FAIL. **Step 3: Implement** validation + the ImGui modal
  per the CreateFlow mock: Name well, Location combo (distinct registry folders +
  the kind default `materials/`|`meshes/`|`sprites/`|`scenes/`), kind fields:
  Material → surface combo (labels `sprite`/`mesh`/`post`); Instance → parent
  picker (materials only, subkind pills, `"materials only · kind-filtered"`
  caption); Create disabled until `ValidateCreateName` passes + required picks
  made. Cancel/× closes.
- [x] **Step 4: Rewire the mints.** In `EditorApp`: a single
  `void BeginCreateAsset(const CreateAssetRequest&);` opens the modal; the panel's
  action fields map to it in `ConsumeBrowserActions`:
  `if (actions.requestCreateKind >= 0) BeginCreateAsset({ (CreateAssetKind)actions.requestCreateKind, actions.createPrefillParent });`
  `ConsumeCreateResult(const CreateAssetResult&)` dispatches:
  Material → `CreateMaterialAt(contentDir/folder/name+".arcmat",
  (MaterialSurface)r.surface)` — **extend `CreateMaterialAt` so the Sprite
  surface writes `data.kind = "sprite"` with the sprite starter graph** (today it
  only mints fullscreen/mesh; follow the existing two branches at
  `EditorAppProject.cpp:700-771` as the template); Instance →
  `CreateInstanceAt(path, r.parent)` (existing, `EditorAppFrame.cpp:605-607`
  consumer). Retire the `ShowSaveFileDialog` launches for `newMaterial`/
  `newMeshMaterial` (`:2210-2233`) — menu items now raise
  `BeginCreateAsset({Material})` / `({Material, prefillSurface=Mesh})`; delete
  the now-unused `m_dialogs.materialNew`/`meshMaterialNew` slots once nothing
  reads them. Assets ▸ Create menu becomes: `Material…`, `Material Instance…`,
  separator, `Mesh…`, `Sprite…`, `Scene…` (last three functional after Task 13 —
  raise the request now; the dialog handles them then).
- [x] **Step 5: Build + run tests + desk smoke** (create a sprite-kind material
  into `materials/`; it registers, opens, appears selected in Browse). Commit —
  `feat(editor): unified CreateAssetRequest + shared create dialog (material/instance)`

---

### Task 13: Create — Mesh / Sprite / Scene flows + retire `+ Mesh`

**Files:**
- Modify: `ArcaneEditor/src/Panels/CreateAssetDialog.cpp`, `AssetsPanel.cpp`,
  `EditorAppFrame.cpp`, `EditorAppProject.cpp`

- [x] **Step 1: Dialog fields:** Sprite → texture picker (textures only) + the
  visible mint-or-reuse notice: if the chosen texture already has a 1:1 derived
  sprite (`model` fold data), show `"a 1:1 sprite already exists"` + an `Open
  existing` button (closes dialog, selects+opens it). Scene → `set as boot`
  checkbox.
- [x] **Step 2: Dispatch:** Mesh → generalize `MintMeshAsset` to take a target
  path (name/folder from the dialog) instead of the hardcoded
  `Content/New Mesh[-N]` (`EditorAppProject.cpp:671-698`); Sprite → mint via the
  `MintOrReuseSpriteForTexture` core but honoring the dialog's name/folder for
  the fresh-mint branch; Scene → scratch registry mint:

```cpp
Astra::Registry scratch;
Arcane::Scene::CreateEmpty(scratch);
std::string err;
if (!Arcane::Scene::SaveSceneFile(target, scratch, Arcane::Guid::Generate(), &err)) { /* ModalErrorQueue */ }
else { m_runtime->RegisterCreatedAsset(target); if (r.setAsBoot) /* existing setBootScene path, EditorAppFrame.cpp:2338 */; }
```

  (Executor: confirm `Scene::CreateEmpty(Astra::Registry&)`'s exact signature
  from `EditorAppScene.cpp:176-186`'s call and that `SaveSceneFile` accepts a
  registry without a live session — it does, it takes `const Astra::Registry&`.)
- [x] **Step 3: Producers:** rail `+` (per-kind request), context `Create ▸`
  submenu, Assets menu entries — all now functional. **Delete the `+ Mesh`
  toolbar button path**: `createMesh` flag removed from actions; its consumer
  block (`EditorAppFrame.cpp:2307-2321`) routes through the dialog instead.
- [x] **Step 4: Build + tests + desk smoke** (each of the five kinds creates,
  registers, lands selected; created scene loads via double-click). Commit —
  `feat(editor): mesh/sprite/scene create flows; + Mesh button retired`

---

### Task 14: Inspector — subkind-filtered material picker

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetPanelModel.hpp` (the heuristic — beside
  `AssetKindFilterForFieldName` once migrated; until Task 15 add it to
  `AssetBrowser.hpp` where that function lives)
- Modify: `ArcaneEditor/src/Panels/InspectorView.cpp` (the material picker draw)
- Test: `ArcaneTests/src/AssetPanelModelTest.cpp`

**Interfaces:**

```cpp
    // Owning-component context for a material-ref field: which MaterialSurface
    // must candidates have? -1 = unfiltered. Extends the field-name-heuristic
    // seam (AssetBrowser.hpp:124-151) until reflection carries attributes.
    [[nodiscard]] int MaterialSurfaceFilterForComponent(std::string_view componentName);
    // "SpriteRenderer" -> Sprite, "MeshRenderer" -> Mesh, else -1.
```

- [x] **Step 1: Failing test** (the two mappings + unknown → -1). **Step 2:** run,
  FAIL. **Step 3:** implement; in `InspectorView.cpp`'s material-guid picker
  population, filter candidates by `surfaceFor` (thread the provider or model in
  through `InspectorServices` — follow how `resolveTexturePreview` reached it,
  `EditorPanels.hpp:338-345`) and render an `AssetPill` with the subkind beside
  each candidate. **Step 4:** build, tests, desk smoke: SpriteRenderer.material
  picker lists only sprite-surface materials with pills. **Step 5:** Commit —
  `feat(editor): subkind-filtered material pickers`

---

### Task 15: Migration — helpers move, `AssetBrowser.*` deleted

**Files:**
- Modify: `ArcaneEditor/src/Panels/AssetPanelModel.hpp` (absorb `AssetKind`,
  `kAssetKindCount`, `AssetKindOf`, `KindIcon`/`KindLabel`, `AssetEntry`,
  `BuildAssetEntries`, `MatchesFilter`, `AssetKindFilterForFieldName`,
  `IsIdentityGuidFieldName`, `kAssetDragType`, `AssetDragPayload`,
  `MaterialSurfaceFilterForComponent` — text-identical moves)
- Delete: `ArcaneEditor/src/Panels/AssetBrowser.hpp`, `AssetBrowser.cpp`
- Modify: every includer (`InspectorView.cpp`, `EditorAppProject.cpp`'s
  `BuildAssetEntries` use at `:210-212` region, `AssetBrowserTest.cpp`,
  `EditorAppFrame.cpp`, premake file lists) — swap includes to
  `Panels/AssetPanelModel.hpp`
- Rename: `ArcaneTests/src/AssetBrowserTest.cpp` → keep the file name (it tests
  surviving helpers) but update its header comment to name the new home

- [x] **Step 1:** move + fix includes + regenerate + build ALL configs
  (Debug/Release). Grep gate: `grep -riw "AssetBrowser" ArcaneEditor ArcaneTests`
  → zero hits outside comments/history (path-exclude sweep method).
- [x] **Step 2:** full `~[gpu]` suite green. **Step 3:** Commit —
  `refactor(editor): retire AssetBrowser.* — helpers live in AssetPanelModel`

---

### Task 16: Gate, re-bless, baselines, desk checklist

- [x] **Step 1:** Build Debug + Release + Dist; run the FULL suite (`~[gpu]` for
  baseline comparison AND one unfiltered run); record fresh assertion counts —
  derive, never recall (run the command, paste from its output).
- [x] **Step 2:** Golden gate: run `golden-gate.ps1`; the editor-ui lane will diff
  against a panel that no longer exists → re-bless (`--bless` pointed at the
  SOURCE project, never the staged tree) and **restage to BOTH hosts**; re-run;
  assert on `gatePassed` + per-lane `verdict` in `golden-gate-summary.json`,
  never the exit code. Beware `golden-gate.ps1` stages `Content/` additively —
  check for stray accumulated files first.
- [x] **Step 3:** Baselines/addendum catch-up commit (spec addendum: LANDED note +
  any deviations), then the plan's ledger updated.
- [x] **Step 4: Desk checklist for the user** (present, don't self-certify):
  side-by-side at 960×620 vs the B+C board and CreateFlow board renders; fold
  behavior on uv_marker; create each of the five kinds; drag row → inspector
  slot; peek tooltip delay; refused-asset row marker + digest count (temporarily
  break a cook to see it); keyboard nav; narrow-dock preview collapse; material
  thumbs live-update on material edit.
- [x] **Step 5: Commit** — `chore(editor): plan-1 gate + baselines catch-up`
  (push only after the user's desk pass, per house convention).

---

## Self-review notes (kept for the executor)

- Spec §3–§8 and §11 are fully covered by Tasks 1–16; Plan-2/-3 sections (§9, §10,
  scene manifest §3.3) are deliberately absent — the structural scan (§3.4) ships
  here (Task 3), the manifest does not.
- The `.arcmesh → material` reference means mesh assets participate in the fold
  graph as ref SOURCES only; nothing folds under a mesh.
- `IsCookPending` defaults to **true when no diagnostic row exists**
  (`EditorAppProject.cpp:521`) — `CookStateOf` must consult it only for kinds that
  actually cook (textures), or every JSON asset would show Queued forever. The
  Task 5 test pins exactly this.
- Sprite-surface material creation (Task 12) is NEW mint behavior (today sprite
  materials are only re-kinded, `EditorPanels.cpp:233-236`) — the executor should
  copy the fullscreen starter-graph branch and adjust the kind string + template,
  checking `MaterialTemplateFile(MaterialSurface::Sprite)` exists
  (`MaterialSource.cpp:330-335` ensures loudly for Mesh only).

---

## COMPLETE — 2026-09-06

All 16 tasks landed on Arcane `main` in place, **`59dd6414..2f5dc391`** (Tasks 1–15)
plus this task's `chore(editor): plan-1 gate + baselines catch-up`. Every task was
reviewed clean (six needed one fix round each, one needed two); the full execution
ledger — dispatches, rulings, fix rounds and every deferred minor — is
`.superpowers/sdd/2026-09-06-asset-manager-plan1/progress.md`.

**Close figures, all DERIVED from their own run's final line** (never recalled):

- Build: `Arcane.slnx` Debug / Release / Dist — **0 warnings, 0 errors** each.
- `ArcaneTests.exe "~[gpu]"`, run FROM the exe dir: Debug **54270 assertions / 1462
  cases** (seed 829257050), Release **54270 / 1462** (seed 1685340276), Dist
  **54202 / 1456** (seed 3577943350). The constant 68/6 Dist gap holds.
- Unfiltered Debug: **116544 / 1495** (seed 3082311851) — the `[gpu]` delta is
  62274 assertions / 33 cases, unchanged by this plan.
- `scripts/automation-baselines.json` re-derived: +288 assertions / +33 cases per
  configuration, matching the raw `TEST_CASE` rise 1475 → 1508.
- Golden gate (Debug, both hosts × both backends): **`gatePassed: true`**, 4 lanes,
  0 red. The editor-ui re-bless was expected (the lane diffed against a panel that
  no longer exists), was verified confined to the Assets panel band before blessing,
  was made against the **source** tree, and was restaged to **both** hosts.
- Gate self-test (`golden-gate.ps1 -SelfTest`, Debug): **PASSED** — all four lanes
  launched and caught the deliberately broken scene by `exitReason=compare-failed`,
  and the tree (source plus both staged copies) restored clean afterwards. The green
  above is therefore from a gate observed *failing* on this tree.

Spec addendum: `docs/specs/2026-09-06-asset-manager-redesign-design.md` **§17
LANDED (Plan 1)** — scope, measured close, the nine recorded deviations, and the
new follow-ups.

**Step 4's checkbox means the desk checklist was WRITTEN, not walked.** It is
`.superpowers/sdd/2026-09-06-asset-manager-plan1/DESK-CHECKLIST.md`, and the desk
pass is the user's. **Nothing is pushed** — the push follows the desk pass, per
house convention.
