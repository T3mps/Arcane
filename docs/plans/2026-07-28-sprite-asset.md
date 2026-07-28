# Sprite Asset (.arcsprite) + SpriteRenderer Rework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `SpriteRenderer::textureId`+`size` with a Guid-referenced `.arcsprite` asset (texture + ppu + sub-rect + pivot); Transform scale becomes the sizing mechanism, matching UE/Unity convention.

**Architecture:** A new engine asset type (`Sprite/SpriteAsset.*`, mirroring `Material/MaterialAsset.*`) plus a `SpriteTable` registry resource (mirroring `SpriteMaterialTable`) resolved by an editor-side `SpriteCache` (mirroring `SpriteMaterialCache`'s sweep/publish flow, minus compilation). The component shrinks to the `PaperSpriteComponent` shape; submission derives size/UVs/pivot from the resolved entry with a 1x1 m fallback.

**Tech Stack:** C++23, nlohmann::json, nvrhi, Astra reflection, Catch2, Dear ImGui (editor).

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-28-sprite-asset-design.md` — decisions there are locked (standalone asset, auto-mint, convention-pure size removal, UE-complete v1 fields, HARD BREAK: no migration/legacy code of any kind).
- **Branch:** continues `arcane-entity-rename` (tip `a644974d`). Entry gate 30256/586 `~[gpu]` — **this arc adds and modifies tests, so the count WILL move**; every task report must state the before/after count and account for the delta exactly (added cases/assertions listed). Full gate green on seed 6 per task; BOTH seeds 6+17 on the final task.
- **Build with the VS 18 toolchain** (`msbuild` on PATH is VS 2022, fails MSB8020):
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal` (from `Arcane/`).
- **Gate from the exe dir:** `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests` then `.\ArcaneTests.exe "~[gpu]" --rng-seed 6` (final task: also `--rng-seed 17`). Never gate from `Arcane/` root (phantom fixture-path failures).
- **New files require `GenerateProjects.bat`** (premake bakes glob expansions into vcxprojs — `Arcane/premake5.lua:171-174`, `:528-600`). Run it in any task that creates files, before building.
- **Editor-only surfaces (EditorPanels.cpp, EditorAppFrame.cpp, SpriteDocument) are NOT in the gate** — exe-timestamp check (`ArcaneEditor.exe` newer than `Arcane/ArcaneEditor/src`) is the compile evidence; UI behavior is desk-verified. Do not invent ImGui tests.
- Exception-free, ASCII-only, UTF-8 no BOM. Comments explain WHY, never overclaim, and cite implementation lines (vendored or first-party) that were actually read. **Never modify vendored code.**
- **Machinery that must survive:** the Inspector AssetRef arm's explicit `!readOnly` drop gate (`EditorPanels.cpp:2063` — `BeginDisabled` does NOT gate drops); `ApplyGuidImmediate`'s one-transaction multi-select fan-out (`EditorPanels.cpp:1772-1784`); `RemoveTexture`'s evict-BEFORE-release order (`Batcher2D.hpp:181-191`); scene JSON stays fully reflection-generic (nothing may special-case SpriteRenderer in serialization).
- **No migration:** old scenes' `size`/`textureId` JSON keys are silently skipped by the generic reader (`ReflectionJson.hpp:379-394` iterates current fields, never JSON keys) — that silence is accepted by decision; do not add warnings for them.

## Verified facts the plan builds on (do not re-derive; re-verify at the step that uses them)

- `TextureTable` has NO writer anywhere; `textures->Resolve()` always returns null today. Deleting it breaks nothing that works.
- Batcher rotation is about the quad CENTER: `QuadCorners` at `Batcher2D.hpp:53-75` (`center = pos + half`, corners rotated about `center`); doc comments `Batcher2D.hpp:47-51,133-135,165`.
- Texture Guid -> GPU: `Assets::GetTexture(const AssetId&)` (`Assets.hpp:81`, impl `Assets.cpp:105-109` -> path load at `:123-204`); resolver installed at `Runtime.cpp:401-402`; precedent consumer `SpriteMaterialCache.cpp:249-253`.
- Publish pattern: `Runtime::SetSpriteMaterials` (`Runtime.cpp:293-298`) + per-frame editor call (`EditorAppFrame.cpp:1025-1026`) + per-frame Guid sweep (`EditorAppFrame.cpp:997-1005`).
- `.arcmat` envelope: top-level `"id"`, `"type":"material"`, `"name"`, payload fields; `SaveMaterialAsset` `MaterialAsset.cpp:114-182`, `LoadMaterialAsset` `:184-368`; registry treats `.json`/`.arcmat`/`.arcscene` as native-id files in `AssetRegistry::AddFile` (`AssetRegistry.cpp:141-192`) — **`.arcsprite` must be added to that native list**.
- New-asset registration without restart: `Runtime::RegisterCreatedAsset` (`Runtime.cpp:420-429`) -> `Project::RegisterAsset` (`Project.cpp:198-227`).
- `EditorDocument` contract: 5 pure virtuals (`EditorDocument.hpp:15-32`); factory routes by extension via `DocumentHost::RegisterFactory` (`DocumentHost.hpp:37-38`), the single existing registration at `EditorApp.cpp:321` (+ peek `:315-320`).
- Inspector AssetRef arm: classified by `f.typeHash == TypeID<Guid>::Hash()` (`InspectorFields.cpp:36,44`); drop payload `AssetDragPayload{guid,kind}` type `"ARCANE_ASSET"` (`AssetBrowser.hpp:40-45`); kind filter from field NAME (`AssetBrowser.hpp:111-121`); write via `ApplyGuidImmediate` (`EditorPanels.cpp:1772-1784`).
- ImGui texture preview: `ImGui::Image((ImTextureID)(uintptr_t)nvrhiTexturePtr, ...)` — backend builds SRV binding sets keyed on the raw pointer (`ImGuiNvrhi.cpp:246-252`); any live `ITexture*` works (`OffscreenCanvas.cpp:105-110` precedent).
- Full `.size`/`.textureId` inventory to update: `PlaygroundGame.cpp:46,54`; tests `EditorCameraTest.cpp:61`, `PickBufferTest.cpp:84`, `SpriteRotationTest.cpp:111,163`, `RenderInterpolationTest.cpp:193,224`; readers `EditorCamera.cpp:43-46,151`, `PickEmit.cpp:38-56`; fixture `SampleProject/Content/scenes/main.arcscene:42-47,85-90,128-133`; display-name golden `EditorInspectorMetaTest.cpp:18`.

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/Arcane/src/Arcane/Sprite/SpriteAsset.hpp` (create) | `SpriteAssetData` + `Save/LoadSpriteAsset` + `ResolvedSpriteGeom`/`ComputeSpriteGeom` (pure). |
| `Arcane/Arcane/src/Arcane/Sprite/SpriteAsset.cpp` (create) | JSON I/O implementations. |
| `Arcane/Arcane/src/Arcane/Project/AssetRegistry.cpp` (modify) | `.arcsprite` joins the native-id extension route. |
| `Arcane/Arcane/src/Arcane/Scene/Components.hpp` (modify) | SpriteRenderer shrinks; reflection updated. |
| `Arcane/Arcane/src/Arcane/Scene/SceneResources.hpp` (modify) | Delete `TextureTable`; add `SpriteEntry` + `SpriteTable`. |
| `Arcane/Arcane/src/Arcane/Scene/RenderSystems.hpp` (modify) | Resolved-entry submission with pivot math. |
| `Arcane/Arcane/src/Arcane/Render/PickEmit.cpp` (modify) | Resolved size for pick OBBs. |
| `Arcane/Arcane/src/Arcane/Base/Runtime.{hpp,cpp}` (modify) | `SetSpriteTable` publish seam. |
| `Arcane/ArcaneEditor/src/EditorCamera.cpp` (modify) | Resolved size for frame-selection bounds. |
| `Arcane/ArcaneEditor/src/SpriteCache.{hpp,cpp}` (create) | Guid -> SpriteEntry resolution, TextureHandle keep-alive, invalidation with `RemoveTexture`. |
| `Arcane/ArcaneEditor/src/AssetBrowser.{hpp,cpp}` (modify) | `AssetKind::Sprite` wiring + "Create Sprite" action. |
| `Arcane/ArcaneEditor/src/EditorPanels.{hpp,cpp}` (modify) | Inspector auto-mint hook (`InspectorServices`). |
| `Arcane/ArcaneEditor/src/EditorApp*.{hpp,cpp}` (modify) | Cache wiring, mint helper, document factory. |
| `Arcane/ArcaneEditor/src/SpriteDocument.{hpp,cpp}` (create) | Compact `.arcsprite` editor document. |
| `Arcane/Tests/src/SpriteAssetTest.cpp` (create) | Round-trip, defaults, geometry math, registry scan. |
| Existing tests (modify) | Per the inventory above. |

---

### Task 1: SpriteAsset engine type + registry integration

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Sprite/SpriteAsset.hpp`, `Arcane/Arcane/src/Arcane/Sprite/SpriteAsset.cpp`
- Modify: `Arcane/Arcane/src/Arcane/Project/AssetRegistry.cpp` (native-extension route, ~`:158-164`)
- Test: `Arcane/Tests/src/SpriteAssetTest.cpp` (create)

**Interfaces produced:**
```cpp
namespace Arcane
{
    struct SpriteAssetData
    {
        Guid        id{};
        std::string name;
        Guid        texture{};                  // source texture asset; nil renders untextured
        float       ppu = 100.0f;               // pixels per meter
        glm::vec2   sourcePos{0.0f, 0.0f};      // sub-rect origin, pixels
        glm::vec2   sourceSize{0.0f, 0.0f};     // sub-rect dims, pixels; (0,0) = whole texture
        glm::vec2   pivot{0.5f, 0.5f};          // normalized
    };
    ARCANE_API bool SaveSpriteAsset(const std::filesystem::path& path, const SpriteAssetData& data);
    ARCANE_API std::optional<SpriteAssetData> LoadSpriteAsset(const std::filesystem::path& path);

    struct ResolvedSpriteGeom { glm::vec2 uvMin; glm::vec2 uvMax; glm::vec2 sizeMeters; };
    ARCANE_API ResolvedSpriteGeom ComputeSpriteGeom(const SpriteAssetData& data,
                                                    std::uint32_t texWidth, std::uint32_t texHeight);
}
```

- [ ] **Step 1: Read the .arcmat precedent end-to-end** — `MaterialAsset.hpp:60-109`, `MaterialAsset.cpp:114-368` (envelope keys, lenient load validation, `dump(2, ' ', false, error_handler_t::replace)` write), and `AssetRegistry.cpp:141-192` (`AddFile` extension routing, `ResolveNativeId` at `:53-76`).

- [ ] **Step 2: Write the failing tests** in `Arcane/Tests/src/SpriteAssetTest.cpp` (Catch2, tags `"[sprite]"`; `TempDir` helper copied from the `MaterialAssetTest.cpp:19-27` convention):

```cpp
// SpriteAssetTest.cpp -- .arcsprite JSON I/O + geometry math. CPU-only, no GPU.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>
#include <Arcane/Project/AssetRegistry.hpp>
#include <filesystem>
#include <fstream>

namespace
{
    std::filesystem::path TempDir(const char* leaf)
    {
        auto d = std::filesystem::temp_directory_path() / "arcane_sprite_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }
}

TEST_CASE("SpriteAsset round-trips every field", "[sprite]")
{
    const auto dir = TempDir("roundtrip");
    Arcane::SpriteAssetData data;
    data.id         = Arcane::Guid::Generate();
    data.name       = "hero";
    data.texture    = Arcane::Guid::Generate();
    data.ppu        = 64.0f;
    data.sourcePos  = {16.0f, 32.0f};
    data.sourceSize = {48.0f, 24.0f};
    data.pivot      = {0.25f, 1.0f};
    REQUIRE(Arcane::SaveSpriteAsset(dir / "hero.arcsprite", data));
    const auto back = Arcane::LoadSpriteAsset(dir / "hero.arcsprite");
    REQUIRE(back.has_value());
    CHECK(back->id == data.id);
    CHECK(back->name == "hero");
    CHECK(back->texture == data.texture);
    CHECK(back->ppu == 64.0f);
    CHECK(back->sourcePos == data.sourcePos);
    CHECK(back->sourceSize == data.sourceSize);
    CHECK(back->pivot == data.pivot);
}

TEST_CASE("SpriteAsset absent fields take defaults", "[sprite]")
{
    const auto dir = TempDir("defaults");
    {
        std::ofstream out(dir / "min.arcsprite", std::ios::binary);
        out << R"({"id":"0123456789abcdef0123456789abcdef","type":"sprite"})";
    }
    const auto back = Arcane::LoadSpriteAsset(dir / "min.arcsprite");
    REQUIRE(back.has_value());
    CHECK_FALSE(back->texture.IsValid());
    CHECK(back->ppu == 100.0f);
    CHECK(back->sourcePos == glm::vec2(0.0f));
    CHECK(back->sourceSize == glm::vec2(0.0f));
    CHECK(back->pivot == glm::vec2(0.5f));
}

TEST_CASE("SpriteAsset load rejects non-sprite json and clamps bad ppu", "[sprite]")
{
    const auto dir = TempDir("reject");
    {
        std::ofstream out(dir / "not.arcsprite", std::ios::binary);
        out << R"({"hello":"world"})";
    }
    CHECK_FALSE(Arcane::LoadSpriteAsset(dir / "not.arcsprite").has_value());
    {
        std::ofstream out(dir / "badppu.arcsprite", std::ios::binary);
        out << R"({"id":"0123456789abcdef0123456789abcdef","type":"sprite","ppu":0.0})";
    }
    const auto back = Arcane::LoadSpriteAsset(dir / "badppu.arcsprite");
    REQUIRE(back.has_value());
    CHECK(back->ppu == 100.0f);   // ppu <= 0 falls back to the default, warned
}

TEST_CASE("ComputeSpriteGeom full texture and sub-rect", "[sprite]")
{
    Arcane::SpriteAssetData data;   // defaults: full rect, ppu 100
    auto g = Arcane::ComputeSpriteGeom(data, 200, 50);
    CHECK(g.uvMin == glm::vec2(0.0f));
    CHECK(g.uvMax == glm::vec2(1.0f));
    CHECK(g.sizeMeters == glm::vec2(2.0f, 0.5f));   // 200/100, 50/100

    data.sourcePos  = {50.0f, 10.0f};
    data.sourceSize = {100.0f, 25.0f};
    g = Arcane::ComputeSpriteGeom(data, 200, 50);
    CHECK(g.uvMin == glm::vec2(0.25f, 0.2f));       // 50/200, 10/50
    CHECK(g.uvMax == glm::vec2(0.75f, 0.7f));       // 150/200, 35/50
    CHECK(g.sizeMeters == glm::vec2(1.0f, 0.25f));  // 100/100, 25/100

    g = Arcane::ComputeSpriteGeom(data, 0, 0);      // no texture dims yet
    CHECK(g.sizeMeters == glm::vec2(1.0f));         // safe fallback, never NaN/0
}

TEST_CASE("AssetRegistry scans and AddFiles .arcsprite as a native asset", "[sprite][project]")
{
    const auto dir = TempDir("registry");
    std::filesystem::create_directories(dir / "Content");
    Arcane::SpriteAssetData data;
    data.id = Arcane::Guid::Generate();
    REQUIRE(Arcane::SaveSpriteAsset(dir / "Content" / "s.arcsprite", data));

    Arcane::AssetRegistry reg;
    reg.ScanContent(dir / "Content", "game");
    CHECK(reg.Resolve(data.id).has_value());
    CHECK(*reg.Resolve(data.id) == "game://s.arcsprite");
}
```

- [ ] **Step 3: Run to verify failure** — the new files don't exist yet, so first `GenerateProjects.bat` is NOT yet needed; expected: the test file itself fails to compile (missing header). This is the RED state.

- [ ] **Step 4: Implement `SpriteAsset.hpp/.cpp`.** Header exactly as in Interfaces above (include `<Arcane/Guid.hpp>`, `<glm/glm.hpp>`, `<filesystem>`, `<optional>`, `<string>`; use the same `ARCANE_API` macro/include as `MaterialAsset.hpp`). Implementation mirrors the material envelope:

```cpp
// SpriteAsset.cpp (shape; follow MaterialAsset.cpp's includes/logging conventions)
bool SaveSpriteAsset(const std::filesystem::path& path, const SpriteAssetData& data)
{
    nlohmann::json doc;
    doc["id"]   = data.id.ToString();
    doc["type"] = "sprite";                       // self-describing, like "material"
    doc["name"] = data.name;
    if (data.texture.IsValid()) doc["texture"] = data.texture.ToString();
    doc["ppu"] = data.ppu;
    // Only write non-default rect/pivot so minimal files stay minimal (absent = default on load).
    if (data.sourcePos != glm::vec2(0.0f))  doc["sourcePos"]  = { data.sourcePos.x,  data.sourcePos.y };
    if (data.sourceSize != glm::vec2(0.0f)) doc["sourceSize"] = { data.sourceSize.x, data.sourceSize.y };
    if (data.pivot != glm::vec2(0.5f))      doc["pivot"]      = { data.pivot.x, data.pivot.y };
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    return out.good();
}

std::optional<SpriteAssetData> LoadSpriteAsset(const std::filesystem::path& path)
{
    // parse with allow_exceptions=false, like LoadMaterialAsset (MaterialAsset.cpp:186-196).
    // Accept only if doc["type"] == "sprite" (stricter than material's structural sniff --
    // sprite has no structurally-unique key, so the type tag IS the discriminator).
    // Read fields with defaults; ppu <= 0 -> ARC_WARN + 100.0f.
    // vec2 fields read as 2-element arrays; wrong shape -> keep default (do not fail the load).
}

ResolvedSpriteGeom ComputeSpriteGeom(const SpriteAssetData& data,
                                     std::uint32_t texWidth, std::uint32_t texHeight)
{
    ResolvedSpriteGeom g{ {0.0f,0.0f}, {1.0f,1.0f}, {1.0f,1.0f} };
    const float ppu = data.ppu > 0.0f ? data.ppu : 100.0f;
    const bool fullRect = data.sourceSize.x <= 0.0f || data.sourceSize.y <= 0.0f;
    if (texWidth == 0 || texHeight == 0)
        return g;                                  // no texture info: 1x1 m, full UV
    const glm::vec2 tex(static_cast<float>(texWidth), static_cast<float>(texHeight));
    const glm::vec2 pos  = fullRect ? glm::vec2(0.0f) : data.sourcePos;
    const glm::vec2 size = fullRect ? tex : data.sourceSize;
    g.uvMin = pos / tex;
    g.uvMax = (pos + size) / tex;
    g.sizeMeters = size / ppu;
    return g;
}
```

- [ ] **Step 5: Add `.arcsprite` to the registry's native route.** In `AssetRegistry.cpp` where `AddFile` routes `.json`/`.arcmat`/`.arcscene` to `ResolveNativeId` (around `:158-164` — re-read the exact shape first), add `.arcsprite` to the same list. Match the existing code style exactly.

- [ ] **Step 6: `GenerateProjects.bat`, build, run the new tests:**
  `.\ArcaneTests.exe "[sprite]" --rng-seed 6` — expected: all pass. Then the full gate seed 6; record the count delta (this task adds cases).

- [ ] **Step 7: Commit** — `git add` the four files only; message: `feat(arcane): .arcsprite sprite asset type with registry integration`.

---

### Task 2: Component rework, SpriteTable, submission, consumers, content

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/Components.hpp` (SpriteRenderer + reflection), `Arcane/Arcane/src/Arcane/Scene/SceneResources.hpp` (delete TextureTable, add SpriteEntry/SpriteTable), `Arcane/Arcane/src/Arcane/Scene/RenderSystems.hpp` (submission), `Arcane/Arcane/src/Arcane/Render/PickEmit.cpp`, `Arcane/Arcane/src/Arcane/Base/Runtime.hpp` + `Runtime.cpp` (SetSpriteTable), `Arcane/ArcaneEditor/src/EditorCamera.cpp`, `Arcane/PlaygroundGame/src/PlaygroundGame.cpp`, `SampleProject/Content/scenes/main.arcscene`
- Test (modify): `SpriteRotationTest.cpp`, `RenderInterpolationTest.cpp`, `PickBufferTest.cpp`, `EditorCameraTest.cpp`, `EditorInspectorMetaTest.cpp`

**Interfaces produced (consumed by Tasks 3-5):**
```cpp
// SceneResources.hpp
struct SpriteEntry
{
    nvrhi::ITexture* texture = nullptr;      // null = untextured (tint quad)
    glm::vec2 uvMin{0.0f, 0.0f};
    glm::vec2 uvMax{1.0f, 1.0f};
    glm::vec2 sizeMeters{1.0f, 1.0f};
    glm::vec2 pivot{0.5f, 0.5f};
};
struct SpriteTable
{
    const std::unordered_map<Guid, SpriteEntry>* sprites = nullptr;
    const SpriteEntry* Resolve(const Guid& g) const;   // null when unset/nil/missing
};
// Runtime.hpp
void SetSpriteTable(const std::unordered_map<Guid, SpriteEntry>* sprites);  // mirrors SetSpriteMaterials
// Components.hpp
struct SpriteRenderer { Guid sprite{}; glm::vec4 tint{1,1,1,1}; int32_t sortingLayer=0;
                        int32_t orderInLayer=0; SpriteShape shape=SpriteShape::Rect; Guid material{}; };
```

- [ ] **Step 1: Re-read** `RenderSystems.hpp:29-135` (current submission), `Batcher2D.hpp:53-75` (center rotation — the pivot math below depends on it), `PickEmit.cpp:38-56`, `EditorCamera.cpp:43-46,151`, and the current reflect block `Components.hpp:176-201`.

- [ ] **Step 2: Write the failing render tests first.** In `SpriteRotationTest.cpp`, convert existing helpers from `.size` to Transform scale (the helper at `:111` gains a scale parameter it writes to the entity's `Transform::scale`; the `40x12` case at `:163` becomes `scale = {40,12}`), and ADD two cases pinning the new behavior (MockBatcher, CPU-only, tag `[render]`):

```cpp
TEST_CASE("Sprite with a resolved SpriteTable entry uses derived size and UVs", "[render][sprite]")
{
    // registry + systems boilerplate: copy the fixture shape at SpriteRotationTest.cpp:95-118.
    std::unordered_map<Arcane::Guid, Arcane::SpriteEntry> table;
    const auto gid = Arcane::Guid::Generate();
    Arcane::SpriteEntry e;                       // texture stays null: Rect path, but geom applies
    e.sizeMeters = {2.0f, 0.5f};
    table.emplace(gid, e);
    reg.SetResource<Arcane::SpriteTable>(Arcane::SpriteTable{ &table });

    // entity: Transform scale (3,4), zoom 1 -> expected rect size (6, 2)
    Arcane::SpriteRenderer sp;
    sp.sprite = gid;
    // ...spawn, run RenderSubmissionSystem, then:
    REQUIRE(batcher.rects.size() == 1);
    CHECK(batcher.rects[0].size == glm::vec2(6.0f, 2.0f));
}

TEST_CASE("Non-center pivot offsets the quad and survives rotation", "[render][sprite]")
{
    // entry: sizeMeters (2,2), pivot (0,0) (top-left). Transform position P, rotation 0, scale 1, zoom 1.
    // pivot (0,0) means the PIVOT sits at P and the quad extends +x/+y:
    //   centerOff = (0.5-0.0, 0.5-0.0) * (2,2) = (1,1); dstPos = P + (1,1) - (1,1) = P.
    CHECK(batcher.rects[0].pos == P);
    // Same entity rotated 90 deg (pi/2): center must orbit the pivot:
    //   rotated centerOff = (-1, 1)  ->  dstPos = P + (-1,1) - (1,1) = P + (-2, 0).
    CHECK(batcher.rects[1].pos == P + glm::vec2(-2.0f, 0.0f));
    CHECK(batcher.rects[1].rotation == Catch::Approx(glm::half_pi<float>()));
}
```
(The MockBatcher already records pos/size/rotation — extend it if a field is missing. Hand-derive the expected numbers in comments exactly as above.)

- [ ] **Step 3: Verify RED** — the new cases fail to compile (`SpriteEntry` unknown). Good.

- [ ] **Step 4: Implement the runtime.**
  - `SceneResources.hpp`: delete `TextureTable` (:80-91); add `SpriteEntry`/`SpriteTable` per Interfaces (Resolve mirrors `SpriteMaterialTable::Resolve` `:103-108`, returning `const SpriteEntry*`).
  - `Components.hpp`: replace the struct body (delete `textureId`+`size`+their comments; add `Guid sprite{}` first) and the reflect block: `sprite` gets `Category "Appearance"` + Tooltip `"The .arcsprite asset this renderer draws. Nil renders an untextured 1x1 m tint quad scaled by the Transform."`; delete the `textureId` and `size` field entries; everything else unchanged.
  - `RenderSystems.hpp`: replace the sizing block (`:70-75`) and Rect arm (`:109-120+`):

```cpp
const SpriteTable* spriteTable = reg.GetResource<SpriteTable>();
// ...inside ForEach, after worldRot/interp:
const SpriteEntry* entry =
    (sprite.shape == SpriteShape::Rect && spriteTable) ? spriteTable->Resolve(sprite.sprite)
                                                       : nullptr;
// Primitives and unresolved sprites draw a 1x1 m base; the sprite asset supplies
// the base for textured rects. Scale (not a component field) is the sizing
// mechanism -- see the 2026-07-28 sprite-asset spec.
const glm::vec2 baseSize = entry ? entry->sizeMeters : glm::vec2(1.0f);
const glm::vec2 pivot    = entry ? entry->pivot      : glm::vec2(0.5f);
const glm::vec2 dstSize  = baseSize * worldScale * ctx->zoom;
const glm::vec2 screenPos = worldPos * ctx->zoom + ctx->cameraOffset;
// The batcher rotates a quad about its CENTER (Batcher2D.hpp:56-57: center = pos + half).
// The entity position is the PIVOT, so place the center at pivot + R(worldRot) * (pivot->center),
// which reduces to dstPos = screenPos - dstSize * 0.5f at the default center pivot.
const glm::vec2 centerOff = (glm::vec2(0.5f) - pivot) * dstSize;
const float cr = std::cos(worldRot), sr = std::sin(worldRot);
const glm::vec2 center(screenPos.x + cr * centerOff.x - sr * centerOff.y,
                       screenPos.y + sr * centerOff.x + cr * centerOff.y);
const glm::vec2 dstPos = center - dstSize * 0.5f;
```
  Circle/Capsule branches keep their current bodies but read the new `dstSize`/`screenPos` (their `entry` is always null by construction above, so they get the 1x1 base). The Rect arm resolves `entry->texture` instead of `TextureTable`; textured draws pass `entry->uvMin/uvMax`; untextured stays `Rect(...)`; the material branch passes the same texture+UVs to `QuadMaterial`.
  - `Runtime.{hpp,cpp}`: `SetSpriteTable` mirroring `SetSpriteMaterials` (`Runtime.cpp:293-298`) exactly, one line different (`SetResource<SpriteTable>(SpriteTable{sprites})`).
  - `PickEmit.cpp:38-56` and `EditorCamera.cpp:43-46`: both fetch `SpriteTable` (registry resource) and use `entry ? entry->sizeMeters : vec2(1)` in place of `sprite.size`; `EditorCamera`'s helper gains the resolved base as a parameter (caller at `:151` resolves).

- [ ] **Step 5: Update the inventory sites.** `PlaygroundGame.cpp:46,54`: delete the `.size` writes and set the entity's `Transform::scale` to the old size values (`{48,48}`, `{20,20}`) at their spawn sites. Tests: `RenderInterpolationTest.cpp:193,224`, `PickBufferTest.cpp:84`, `EditorCameraTest.cpp:61` — same conversion (scale instead of size; keep every expected value identical, since submission multiplied size by scale and the tests used scale 1). `EditorInspectorMetaTest.cpp:18`: replace the `"textureId"` golden with `CHECK(DeriveDisplayName("sourcePos") == "Source Pos");` (dead field names make stale goldens). `SampleProject/Content/scenes/main.arcscene:42-47,85-90,128-133`: for each of the three entities, delete the `"textureId"` key and move the `"size"` values into the entity's Transform `"scale"` (multiply into any existing non-1 scale), keeping visual size identical.

- [ ] **Step 6: Build, gate seed 6, exe timestamp.** Expected failures BEFORE the fixes in Step 5, none after. Account for the count delta (2 new cases; modified assertions). `SceneJsonTest` must stay green untouched (it never referenced the dead fields). Note in the report: `SceneSliceTest [gpu]` not run in the dev gate — its sprites become 1x1 default; `quads >= 2` still holds (desk/CI evidence).

- [ ] **Step 7: Commit** — `feat(arcane): SpriteRenderer references .arcsprite; size and TextureTable removed`.

---

### Task 3: Editor SpriteCache + frame wiring

**Files:**
- Create: `Arcane/ArcaneEditor/src/SpriteCache.hpp`, `SpriteCache.cpp`
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp` (member), `EditorApp.cpp` (construction ~`:326-342`), `EditorAppFrame.cpp` (sweep ~`:997-1005`, publish ~`:1025-1026`)

**Interfaces produced (consumed by Tasks 4-5):**
```cpp
namespace Arcane::Editor
{
    class SpriteCache
    {
    public:
        struct Services
        {
            Arcane::Assets*   assets  = nullptr;               // GetTexture(AssetId)
            Arcane::Batcher2D* batcher = nullptr;              // RemoveTexture on eviction
            std::function<std::optional<std::filesystem::path>(const Arcane::AssetId&)> resolveAsset;
        };
        explicit SpriteCache(Services services);
        void Request(const Arcane::Guid& id);                  // idempotent; loads + resolves once
        void Invalidate(const Arcane::Guid& id);               // drop entry; RemoveTexture FIRST
        const std::unordered_map<Arcane::Guid, Arcane::SpriteEntry>& Table() const;
    private:
        Services m_services;
        std::unordered_map<Arcane::Guid, Arcane::SpriteEntry> m_table;
        std::unordered_map<Arcane::Guid, nvrhi::TextureHandle> m_handles;  // keep-alive refs
    };
}
```

- [ ] **Step 1: Read the precedent trio** — `SpriteMaterialCache.hpp` (Services shape `:51`, `Table()` `:77`), its construction (`EditorApp.cpp:326-342`), the per-frame material sweep (`EditorAppFrame.cpp:997-1005`) and publish (`:1025-1026`); plus `Assets.hpp:79-81` and `Batcher2D.hpp:181-192`.

- [ ] **Step 2: Implement `SpriteCache`.** `Request(id)`: return early if `m_table` contains `id` or `id` is nil; `resolveAsset(AssetId::FromGuid(id))` -> `LoadSpriteAsset(path)`; on failure, insert a DEFAULT `SpriteEntry` (1x1 untextured — a broken asset renders as the placeholder, and the negative result is cached so a bad file is not re-parsed every frame; note this in a comment). On success: `assets->GetTexture(AssetId::FromGuid(data.texture))` when `texture` is valid; if a texture handle arrives, `ComputeSpriteGeom(data, desc.width, desc.height)` from `texture->getDesc()`; build the `SpriteEntry{tex.Get(), g.uvMin, g.uvMax, g.sizeMeters, data.pivot}` and stash the handle in `m_handles`. `Invalidate(id)`: if an entry exists — `m_services.batcher->RemoveTexture(entry.texture)` FIRST (evict-before-release, `Batcher2D.hpp:181-191`), then erase from both maps.

- [ ] **Step 3: Wire the editor host.** `EditorApp.hpp`: `std::unique_ptr<Arcane::Editor::SpriteCache> m_sprites;`. `EditorApp.cpp` beside the material cache construction: services from the same sources (`assets = &m_runtime->AssetsFacade()`, the batcher the material cache uses, `resolveAsset` lambda over `Project::ResolveAsset`). `EditorAppFrame.cpp`: in the existing sweep loop that visits every `SpriteRenderer` for materials (`:997-1005`), also `m_sprites->Request(sr.sprite)` for valid Guids; beside the material publish (`:1025-1026`): `if (m_sprites) m_runtime->SetSpriteTable(&m_sprites->Table());`.

- [ ] **Step 4: Build (GenerateProjects first — new files), full gate seed 6, exe timestamp.** The gate does not compile EditorApp/SpriteCache — state that plainly; the compile evidence is the editor exe relink. Desk item recorded for the arc list: assign a sprite in the editor, see the textured quad.

- [ ] **Step 5: Commit** — `feat(editor): SpriteCache resolves .arcsprite Guids into the runtime SpriteTable`.

---

### Task 4: AssetKind::Sprite + Create Sprite + Inspector auto-mint

**Files:**
- Modify: `Arcane/ArcaneEditor/src/AssetBrowser.hpp` (enum `:26-36`, `AssetKindOf` `:49-71`, heuristic `:111-121`, actions struct `:158-171`), `AssetBrowser.cpp` (KindIcon `:16-29`, KindLabel `:31-44`, texture-row context menu near `:112-124`), `EditorPanels.hpp`/`EditorPanels.cpp` (InspectorServices + drop-mint in the AssetRef arm `~:2003-2090`), `EditorAppProject.cpp` (mint helper beside `CreateInstanceAt` `:156-176`), `EditorAppFrame.cpp` (action consumer `~:1103-1140`, panel call site)
- Test (modify): `Arcane/Tests/src/AssetBrowserTest.cpp`

- [ ] **Step 1: Write the failing pure-logic tests** (AssetBrowserTest.cpp, tag `[editor]`):

```cpp
CHECK(AssetKindOf("game://sprites/hero.arcsprite") == AssetKind::Sprite);
CHECK(AssetKindFilterForFieldName("sprite") == static_cast<int>(AssetKind::Sprite));
CHECK(AssetKindFilterForFieldName("material") == static_cast<int>(AssetKind::Material)); // order preserved
```

- [ ] **Step 2: RED, then wire the enum.** `AssetKind::Sprite` inserted before `Other`; `kAssetKindCount = 8`; `AssetKindOf`: `.arcsprite` -> Sprite; heuristic: after the "texture" branch, `if (lower.find("sprite") != std::string::npos) return (int)AssetKind::Sprite;` (AFTER material/texture: a name like `spriteMaterial` must stay Material — say so in a comment); `KindLabel`: `"Sprite"`; `KindIcon`: `ICON_LC_STICKER` if defined in `IconsLucide.h` (grep it), else `ICON_LC_GHOST` (both are Lucide names; verify the chosen one exists before using it). Run the `[editor]` tests green.

- [ ] **Step 3: Mint helper** in `EditorAppProject.cpp` beside `CreateInstanceAt`:

```cpp
// Reuse-or-mint policy (sprite-asset spec, Section 3): exactly one existing
// .arcsprite referencing this texture -> reuse it; zero or several -> mint a
// fresh sibling (never guess among duplicates).
Arcane::Guid EditorApp::MintOrReuseSpriteForTexture(const Arcane::Guid& textureGuid)
{
    const Arcane::Project* project = /* current project accessor used elsewhere in this file */;
    if (!project || !textureGuid.IsValid()) return {};
    Arcane::Guid unique{}; int matches = 0;
    for (const auto& [guid, mount] : project->Registry().All())
    {
        if (Arcane::Editor::AssetKindOf(mount) != Arcane::Editor::AssetKind::Sprite) continue;
        const auto p = project->ResolveAsset(Arcane::AssetId::FromGuid(guid));
        if (!p) continue;
        const auto data = Arcane::LoadSpriteAsset(*p);
        if (data && data->texture == textureGuid) { ++matches; unique = guid; }
    }
    if (matches == 1) return unique;

    const auto texPath = project->ResolveAsset(Arcane::AssetId::FromGuid(textureGuid));
    if (!texPath) return {};
    std::filesystem::path target = texPath->parent_path() / (texPath->stem().string() + ".arcsprite");
    for (int i = 1; std::filesystem::exists(target); ++i)   // never clobber an existing file
        target = texPath->parent_path() / (texPath->stem().string() + "-" + std::to_string(i) + ".arcsprite");

    Arcane::SpriteAssetData data;
    data.id      = Arcane::Guid::Generate();
    data.name    = target.stem().string();
    data.texture = textureGuid;
    if (!Arcane::SaveSpriteAsset(target, data)) return {};
    m_runtime->RegisterCreatedAsset(target);                // live-registry insert, Runtime.cpp:420-429
    return data.id;
}
```
(Resolve the project accessor from how `CreateInstanceAt` gets its paths — read the surrounding file; `Registry()` is on `Project`, used by `AssetBrowser` via `project->Registry()`.)

- [ ] **Step 4: Browser action.** `AssetBrowserActions` gains `Arcane::Guid createSpriteFrom{};` — set from a new context menu on Texture rows (`if (e.kind == AssetKind::Texture && ImGui::BeginPopupContextItem())` -> `MenuItem("Create Sprite")`), mirroring the Material menu at `AssetBrowser.cpp:112-117`. Consumer in `EditorAppFrame.cpp` beside the others (`:1103-1140`): `if (browserActions.createSpriteFrom.IsValid()) { if (auto g = MintOrReuseSpriteForTexture(browserActions.createSpriteFrom); g.IsValid()) if (auto p = /*project*/->ResolveAsset(Arcane::AssetId::FromGuid(g))) m_documents.OpenPath(*p); }` — minting from the browser also OPENS the new sprite (the user asked for it explicitly; the Inspector auto-mint below does NOT open a document).

- [ ] **Step 5: Inspector auto-mint.** `EditorPanels.hpp`: `struct InspectorServices { std::function<Arcane::Guid(const Arcane::Guid&)> mintSpriteForTexture; };` and `DrawInspectorPanel` gains a trailing `const InspectorServices* services = nullptr` parameter. In the AssetRef arm's drop handler (`EditorPanels.cpp:2063-2072`), after the existing exact-kind accept, add the one cross-kind case:

```cpp
else if (kindFilter == static_cast<int>(Arcane::Editor::AssetKind::Sprite)
         && payload->kind == Arcane::Editor::AssetKind::Texture
         && services && services->mintSpriteForTexture)
{
    // Dropping a TEXTURE on a sprite field auto-mints (or reuses) the .arcsprite --
    // Unity's drop-and-go on UE's explicit-asset storage (sprite-asset spec, Section 3).
    // The mint happens before the transaction opens, so undo carries only the Guid edit;
    // undo does NOT delete the minted file (a created asset outlives the edit, like any file).
    if (const Arcane::Guid minted = services->mintSpriteForTexture(payload->guid); minted.IsValid())
        ApplyGuidImmediate(f, targets, minted, undo, descriptions);
}
```
Use the ACTUAL local names at the site (`ApplyGuidImmediate`'s real signature is at `EditorPanels.cpp:1772` — match it; the existing same-kind accept at `:2068-2069` shows the exact call shape). The `!readOnly` outer gate at `:2063` already covers this branch — do not restructure it. `EditorAppFrame.cpp` passes `&m_inspectorServices` (member built once: `mintSpriteForTexture = [this](const Arcane::Guid& t){ return MintOrReuseSpriteForTexture(t); }`).

- [ ] **Step 6: Build, `[editor]` tests + full gate seed 6, exe timestamp, commit** — `feat(editor): sprite asset kind, Create Sprite action, and texture-drop auto-mint`.

---

### Task 5: SpriteDocument + factory + arc close-out

**Files:**
- Create: `Arcane/ArcaneEditor/src/SpriteDocument.hpp`, `SpriteDocument.cpp`
- Modify: `Arcane/ArcaneEditor/src/EditorApp.cpp` (factory registration beside `:321`)

- [ ] **Step 1: Read** `EditorDocument.hpp:15-32` (the five virtuals + optional Tick), the `.arcmat` factory+peek registration (`EditorApp.cpp:303-321`), and the preview convention (`ImGui::Image((ImTextureID)(uintptr_t)tex, ...)`, binding-set cache keyed on the pointer, `ImGuiNvrhi.cpp:246-252`).

- [ ] **Step 2: Implement `SpriteDocument`** (`final : public EditorDocument`). Construction: path + `Services { Arcane::Assets* assets; std::function<void(const Arcane::Guid&)> invalidateSprite; }`; loads `SpriteAssetData` via `LoadSpriteAsset` (failure -> factory returns null, matching how the material factory bails). Virtuals: `Title()` = asset name (fallback: file stem); `AssetGuid()` = `data.id`; `Dirty()` = flag set by any edit; `Save()` = `SaveSpriteAsset(path, data)`, on success clear dirty and call `invalidateSprite(data.id)` so the SpriteCache re-resolves next frame (this is what makes edits show up in the viewport); `Draw(bool& requestClose)`:
  - Fields: `DragFloat("Pixels Per Meter", &data.ppu, 0.5f, 1.0f, 4096.0f)` (clamp > 0); `DragFloat2` for sourcePos/sourceSize (min 0); `DragFloat2` pivot (clamp [0,1]); each edit sets the dirty flag. Texture row: read-only name + the texture Guid (changing the texture is done by re-minting; keep v1 minimal — note it in a comment).
  - Preview: `assets->GetTexture(AssetId::FromGuid(data.texture))` (cached facade call, cheap per frame); if non-null, `ImGui::Image` scaled to fit the available width, then outline the sub-rect over it with `GetWindowDrawList()->AddRect` (map `sourcePos/sourceSize` through the drawn image's screen rect; full-rect when sourceSize is (0,0)).
  - Ctrl+S / a Save button both call `Save()`; `requestClose` per the host's existing pattern (read how ShaderEditorDocument sets it — mirror only the close/dirty-prompt minimum the host requires).

- [ ] **Step 3: Register the factory** beside the `.arcmat` route (`EditorApp.cpp:321`): extension `".arcsprite"`, factory constructs `SpriteDocument` with services (`assets = &m_runtime->AssetsFacade()`, `invalidateSprite = [this](const Guid& g){ if (m_sprites) m_sprites->Invalidate(g); }`), peek = `LoadSpriteAsset(path) -> data->id` (mirror the material peek `:315-320`).

- [ ] **Step 4: `GenerateProjects.bat`, build, exe timestamp; FULL gate BOTH seeds** (`--rng-seed 6` and `--rng-seed 17`), record final counts + full delta accounting vs the 30256/586 entry baseline.

- [ ] **Step 5: Commit** — `feat(editor): SpriteDocument editor for .arcsprite assets`.

---

## Self-Review

**Spec coverage:** Section 1 (asset fields/defaults/additive JSON) -> Task 1. Section 2 (component shrink, SpriteTable, derived size, pivot math, primitives-by-scale, TextureTable deletion, unresolved fallback) -> Task 2 (+ Task 3 for population). Section 3 (AssetKind wiring, auto-mint reuse-or-mint, Create Sprite action, SpriteDocument, AssetRef arm reuse) -> Tasks 4-5. Section 4 (hard break, content re-key, test split) -> Task 2 Step 5 + per-task gate/timestamp evidence. Non-goals respected: no flip, no slicing tooling, no viewport drop, no draw modes, no migration.

**Placeholders:** none — every code step shows the code or names the exact precedent lines to mirror (`SetSpriteTable` = `SetSpriteMaterials` one-line variant; registry route = extend the existing list; the two "read the surrounding file" notes resolve accessor names, not designs).

**Type consistency:** `SpriteAssetData`/`SaveSpriteAsset`/`LoadSpriteAsset`/`ComputeSpriteGeom`/`ResolvedSpriteGeom` (Task 1) are consumed by those exact names in Tasks 2 (geometry), 3 (cache), 4 (mint), 5 (document). `SpriteEntry`/`SpriteTable`/`SetSpriteTable` (Task 2) consumed by Task 3's `Table()` publish and the tests. `MintOrReuseSpriteForTexture` (Task 4 Step 3) is the same symbol consumed in Steps 4-5. `SpriteCache::Invalidate` (Task 3) is the same symbol Task 5's `invalidateSprite` lambda calls.

## Desk-Verify (arc acceptance)

1. Drop a PNG-backed texture from the Asset Browser onto a SpriteRenderer's Sprite field: an `.arcsprite` appears beside the texture, the field fills, the sprite renders at `texturePixels/100` meters. Drop the same texture on another entity: the SAME asset is reused (no duplicate file).
2. "Create Sprite" on a texture row mints and opens the SpriteDocument; editing ppu/rect/pivot + Save updates the rendered sprite next frame (cache invalidation path).
3. Non-center pivot: set pivot (0,0), rotate the entity — the quad orbits the entity position rather than spinning in place.
4. Undo after a texture-drop auto-mint: the Guid edit reverts; the minted file remains (documented decision).
5. A nil/never-assigned sprite renders the 1x1 m tint quad scaled by the Transform; Circle/Capsule size purely by scale (drag scale, watch the disc grow).
6. PlaygroundGame orbit scene looks unchanged (sizes folded into scale); SampleProject scene loads with no size/textureId keys and unchanged proportions.
7. Editor camera "frame selection" still frames a textured sprite tightly (resolved size, not 1x1) and pick-clicks land on the visible quad.
