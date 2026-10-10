// The shared asset-reference field (spec 2026-09-30 s4.2). Part 1: the PURE
// halves (DescribeAssetRef / DecideAssetRefDrop / AssetRefCandidates) over a
// REAL project with three assets (AssetInspectorSourceTest.cpp's Project::
// Create shape) and a model built with faked providers. Part 2 (T2-B2) drives
// the cell through device-less ImGui frames; part 3 (T2-B3) the entity page.
#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetPanelModel.hpp"
#include "Panels/AssetReferenceField.hpp"
#include "Panels/InspectorView.hpp"     // DrawReflectedComponent, ReflectedComponentArgs, InspectorServices
#include "Widgets/EditorWidgets.hpp"    // FieldGrid, TableRowHeight
#include "Widgets/UiMetrics.hpp"        // Ui::ScopedMetrics
#include "Widgets/IconsLucide.h"
#include "Widgets/PropertyGrid.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Reflection/TypeMeta.hpp>
#include <Astra/Registry/Registry.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <glm/vec2.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Arcane::Editor;
namespace fs = std::filesystem;

namespace
{
    fs::path WriteFile(const fs::path& dir, const char* name, const std::string& text)
    {
        fs::path p = dir / name;
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }

    Arcane::Guid GuidForPath(const std::vector<std::pair<Arcane::Guid, std::string>>& all, std::string_view mountPath)
    {
        for (const auto& [guid, path] : all)
            if (path == mountPath) return guid;
        return Arcane::Guid{};
    }

    // brick.png (Texture), wall.arcmat (Material, CONFIRMED Mesh surface) and
    // sub/wall.arcmat (Material, surface UNKNOWN): two "wall"s so the
    // name-then-path order is observable.
    struct RefFixture
    {
        fs::path root;
        std::optional<Arcane::Project> project;
        AssetPanelModel model;
        Arcane::Guid gBrick, gWall, gSubWall;
        std::unordered_map<Arcane::Guid, Arcane::MaterialSurface> surfaces;
        AssetPanelProviders providers;   // kept: a case that edits `surfaces` rebuilds the model with them
        AssetRefServices services;

        explicit RefFixture(const char* name) : root(fs::temp_directory_path() / name)
        {
            std::error_code ec;
            fs::remove_all(root, ec);
            REQUIRE(Arcane::Project::Create(root, "AssetRef").has_value());
            WriteFile(root / "Content", "brick.png", "not a real png, just bytes");   // sidecar-minted guid
            WriteFile(root / "Content", "wall.arcmat",
                      R"({"id":"a5500002-0002-4002-8002-000000000002","type":"material","kind":"mesh"})");
            fs::create_directories(root / "Content" / "sub");
            WriteFile(root / "Content" / "sub", "wall.arcmat",
                      R"({"id":"a5500003-0003-4003-8003-000000000003","type":"material","kind":"fullscreen"})");
            project = Arcane::Project::Open(root);
            REQUIRE(project.has_value());
            const auto all = project->Registry().All();
            gBrick = GuidForPath(all, "game://brick.png");
            gWall = GuidForPath(all, "game://wall.arcmat");
            gSubWall = GuidForPath(all, "game://sub/wall.arcmat");
            REQUIRE(gBrick.IsValid());
            REQUIRE(gWall.IsValid());
            REQUIRE(gSubWall.IsValid());
            surfaces[gWall] = Arcane::MaterialSurface::Mesh;   // sub/wall stays unknown (nullopt)
            AssetPanelProviders& p = providers;
            p.surfaceFor = [this](const Arcane::Guid& g) -> std::optional<Arcane::MaterialSurface>
            {
                const auto it = surfaces.find(g);
                return it == surfaces.end() ? std::nullopt : std::optional<Arcane::MaterialSurface>(it->second);
            };
            p.refsFor = [](const Arcane::Guid&) -> std::optional<std::vector<Arcane::AssetRef>>
            { return std::vector<Arcane::AssetRef>{}; };
            p.cookStateFor = [](const Arcane::Guid&) { return CookState::Cooked; };
            model.MarkAllDirty();
            REQUIRE(model.RebuildIfDirty(&project->Registry(), p));
            REQUIRE(model.Find(gWall) != nullptr);
            REQUIRE(model.Find(gWall)->surface.has_value());
            services.model = &model;
            services.project = [this]() -> const Arcane::Project* { return &*project; };
        }
        ~RefFixture()
        {
            project.reset();
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    };

    std::size_t IndexOf(const std::vector<const AssetPanelEntry*>& rows, const Arcane::Guid& g)
    {
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i]->guid == g) return i;
        return rows.size();
    }
}

TEST_CASE("DecideAssetRefDrop: kind match sets, read-only and identity refuse, a texture mints only on a minting sprite field", "[editor][assetref]")
{
    const Arcane::Guid g = Arcane::Guid::Generate();
    const AssetDragPayload mat{ g, AssetKind::Material }, tex{ g, AssetKind::Texture }, spr{ g, AssetKind::Sprite };
    AssetRefArgs material;
    material.kindFilter = static_cast<int>(AssetKind::Material);
    CHECK(DecideAssetRefDrop(mat, material) == AssetRefDropVerdict::Set);        // match
    CHECK(DecideAssetRefDrop(tex, material) == AssetRefDropVerdict::Refuse);     // mismatch: texture on a material
    AssetRefArgs any;
    CHECK(DecideAssetRefDrop(tex, any) == AssetRefDropVerdict::Set);             // any kind
    AssetRefArgs ro = any;
    ro.readOnly = true;
    CHECK(DecideAssetRefDrop(tex, ro) == AssetRefDropVerdict::Refuse);
    AssetRefArgs identity = any;
    identity.identityGuid = true;                                                 // the Identity::id drop (InspectorView.cpp:1142-1155)
    CHECK(DecideAssetRefDrop(tex, identity) == AssetRefDropVerdict::Refuse);
    AssetRefArgs sprite;
    sprite.kindFilter = static_cast<int>(AssetKind::Sprite);
    CHECK(DecideAssetRefDrop(spr, sprite) == AssetRefDropVerdict::Set);
    CHECK(DecideAssetRefDrop(tex, sprite) == AssetRefDropVerdict::Refuse);       // no mint without allowTextureMint
    sprite.allowTextureMint = true;
    CHECK(DecideAssetRefDrop(tex, sprite) == AssetRefDropVerdict::MintSprite);
}

TEST_CASE("AssetRefCandidates: kind filter, confirmed-surface exclusion, unknown kept, name-then-path order, search on name and path", "[editor][assetref]")
{
    RefFixture fx("arcane_assetref_candidates_test");
    const int material = static_cast<int>(AssetKind::Material);
    const auto textures = AssetRefCandidates(fx.model, static_cast<int>(AssetKind::Texture), -1, "");
    REQUIRE(textures.size() == 1);
    CHECK(textures[0]->guid == fx.gBrick);

    const auto mats = AssetRefCandidates(fx.model, material, -1, "");
    REQUIRE(mats.size() == 2);
    CHECK(mats[0]->guid == fx.gSubWall);                  // both "wall": "game://sub/wall..." < "game://wall..."
    CHECK(mats[1]->guid == fx.gWall);

    const auto meshSurface = AssetRefCandidates(fx.model, material, static_cast<int>(Arcane::MaterialSurface::Mesh), "");
    CHECK(meshSurface.size() == 2);                       // wall matches, sub/wall unknown -> kept
    const auto spriteSurface = AssetRefCandidates(fx.model, material, static_cast<int>(Arcane::MaterialSurface::Sprite), "");
    REQUIRE(spriteSurface.size() == 1);                   // wall is CONFIRMED Mesh: out
    CHECK(spriteSurface[0]->guid == fx.gSubWall);

    const auto all = AssetRefCandidates(fx.model, -1, -1, "");
    CHECK(IndexOf(all, fx.gBrick) < IndexOf(all, fx.gSubWall));   // "brick" < "wall"
    CHECK(IndexOf(all, fx.gSubWall) < IndexOf(all, fx.gWall));
    CHECK(IndexOf(all, fx.gWall) < all.size());

    const auto byName = AssetRefCandidates(fx.model, -1, -1, "BRI");       // case-insensitive
    REQUIRE(byName.size() == 1);
    CHECK(byName[0]->guid == fx.gBrick);
    const auto byPath = AssetRefCandidates(fx.model, -1, -1, "sub/");
    REQUIRE(byPath.size() == 1);
    CHECK(byPath[0]->guid == fx.gSubWall);
}

TEST_CASE("DescribeAssetRef: identity, mixed, nil, resolved, unknown-to-model, dangling, tombstone, null services", "[editor][assetref]")
{
    RefFixture fx("arcane_assetref_describe_test");
    AssetRefArgs a;

    a.guid = fx.gBrick;
    a.identityGuid = true;                                // never resolved, never browsable
    AssetRefDisplay d = DescribeAssetRef(a, fx.services);
    CHECK(d.text == fx.gBrick.ToString());
    CHECK_FALSE(d.browsable);
    CHECK_FALSE(d.dangling);
    a.identityGuid = false;

    a.mixed = true;
    CHECK(DescribeAssetRef(a, fx.services).text == "--");
    a.identityGuid = true;                                // Identity::id across a multi-selection: "--", never the primary's id
    CHECK(DescribeAssetRef(a, fx.services).text == "--");
    a.identityGuid = false;
    a.mixed = false;

    AssetRefArgs nil;
    CHECK(DescribeAssetRef(nil, fx.services).text == "(none)");

    d = DescribeAssetRef(a, fx.services);                 // resolved
    CHECK(d.text == "brick.png");
    CHECK(d.tooltip == "game://brick.png");
    CHECK(d.browsable);
    CHECK_FALSE(d.dangling);
    CHECK(d.kind == AssetKind::Texture);

    AssetPanelModel empty;                                // the model does not know the guid yet
    AssetRefServices early = fx.services;
    early.model = &empty;
    d = DescribeAssetRef(a, early);
    CHECK(d.text == "brick.png");                         // the mount path's last segment
    CHECK(d.kind == AssetKind::Texture);

    AssetRefArgs gone;
    gone.guid = *Arcane::Guid::FromString("deadbeef-0000-4000-8000-000000000001");
    d = DescribeAssetRef(gone, fx.services);
    CHECK(d.dangling);
    CHECK(d.text == gone.guid.ToString() + " (missing)");
    CHECK_FALSE(d.browsable);
    AssetRefServices tomb = fx.services;
    tomb.tombstoneName = [](const Arcane::Guid&) -> std::optional<std::string> { return std::string("old_wall.arcmat"); };
    d = DescribeAssetRef(gone, tomb);
    CHECK(d.text == "old_wall.arcmat (missing)");
    CHECK(d.tooltip.find(gone.guid.ToString()) != std::string::npos);

    const AssetRefServices none{};                        // headless: raw guid, never dangling
    d = DescribeAssetRef(a, none);
    CHECK(d.text == fx.gBrick.ToString());
    CHECK_FALSE(d.browsable);
    d = DescribeAssetRef(gone, none);
    CHECK(d.text == gone.guid.ToString());
    CHECK_FALSE(d.dangling);
}

// ---- Part 2: the cell, device-less (the GridHarness shape) -------------------
namespace
{
    ImGuiWindow* PopupWindow(ImGuiID popupId)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "##Popup_%08x", popupId);   // BeginPopupEx's own naming (imgui.cpp:13155)
        return ImGui::FindWindowByName(name);
    }

    // Window "t" (392 x 330, the 1080p Assets-only Inspector) holds two cells:
    // "Material" (editable) and "Locked" (read-only, gBrick). Window "src" off
    // to the right is a drag source that offers `drag` while it is set.
    struct CellHarness
    {
        RefFixture& fx;
        const AssetRefServices* services;
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        AssetRefArgs editable, locked;
        std::vector<AssetRefEdit> editableEdits, lockedEdits;
        std::string editableLog, lockedLog;
        float editableTop = 0.0f, lockedTop = 0.0f, cellMinX = 0.0f, cellWidth = 0.0f;
        ImGuiID activate = 0;
        std::optional<AssetDragPayload> drag;
        ImVec2 srcCentre{};
        int opens = 0, reveals = 0;
        bool browserOpen = true;

        explicit CellHarness(RefFixture& f) : fx(f), services(&f.services)
        {
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 1024.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            fx.services.open = [this](const Arcane::Guid&) { ++opens; };
            fx.services.reveal = [this](const Arcane::Guid&) { ++reveals; };
            fx.services.canReveal = [this] { return browserOpen; };
            editable.kindFilter = static_cast<int>(AssetKind::Texture);
            locked.readOnly = true;
            locked.guid = fx.gBrick;
        }
        ~CellHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }

        // A cell's item id: window "t" -> PushID(row) -> the cell's PushID("##value") -> item.
        static ImGuiID CellItem(const char* row, const char* item)
        {
            return ImHashStr(item, 0, ImHashStr("##value", 0, ImHashStr(row, 0, ImHashStr("t"))));
        }

        void Cell(const char* row, const AssetRefArgs& args, float& top, std::string& log, std::vector<AssetRefEdit>& edits)
        {
            ImGui::PushID(row);
            top = ImGui::GetCursorScreenPos().y;
            ImGui::LogToBuffer();
            const AssetRefEdit e = AssetReferenceValue("##value", args, *services);
            log = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
            if (e.op != AssetRefEdit::Op::None) edits.push_back(e);
            ImGui::PopID();
        }

        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            if (activate) { ImGui::ActivateItemByID(activate); activate = 0; }   // lands on the NEXT frame
            ImGui::SetNextWindowPos(ImVec2(700.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(200.0f, 100.0f), ImGuiCond_Always);
            ImGui::Begin("src");
            ImGui::Button("drag me");
            srcCentre = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                               (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
            if (drag && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
            {
                ImGui::SetDragDropPayload(kAssetDragType, &*drag, sizeof(AssetDragPayload));
                ImGui::EndDragDropSource();
            }
            ImGui::End();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(392.0f, 330.0f), ImGuiCond_Always);
            ImGui::Begin("t");
            cellMinX = ImGui::GetCursorScreenPos().x;
            cellWidth = ImGui::GetContentRegionAvail().x;
            Cell("Material", editable, editableTop, editableLog, editableEdits);
            Cell("Locked", locked, lockedTop, lockedLog, lockedEdits);
            ImGui::End();
            ImGui::Render();
        }
        void Frames(int n) { for (int i = 0; i < n; ++i) Frame(); }

        // Press the source, move past MouseDragThreshold (6 px), hover the
        // target (accepted = preview), one more frame (the delivery is armed),
        // release (delivered).
        void DragTo(ImVec2 target)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(srcCentre.x, srcCentre.y); Frame();
            io.AddMouseButtonEvent(0, true); Frame();
            io.AddMousePosEvent(srcCentre.x + 20.0f, srcCentre.y); Frame();
            io.AddMousePosEvent(target.x, target.y); Frame();
            Frame();
            io.AddMouseButtonEvent(0, false); Frame();
            Frame();
            drag.reset();
        }
    };
}

TEST_CASE("AssetReferenceValue: the chevron opens the picker below the cell; a candidate sets once, (none) clears", "[editor][assetref]")
{
    RefFixture fx("arcane_assetref_cell_pick_test");
    CellHarness h(fx);
    h.Frames(2);
    h.activate = CellHarness::CellItem("Material", ICON_LC_CHEVRON_DOWN "##pick");
    h.Frames(3);                                          // the press lands, the popup appears, it is placed
    ImGuiWindow* popup = PopupWindow(CellHarness::CellItem("Material", "##assetpick"));
    REQUIRE(popup != nullptr);
    REQUIRE(popup->Active);
    CHECK(popup->Pos.y >= h.editableTop + ImGui::GetFrameHeight() - 0.5f);   // below the cell (BeginPopupBelow)
    CHECK(popup->Size.x >= h.cellWidth - 0.5f);                              // at least as wide as the cell

    h.activate = ImHashStr("##row", 0, ImHashStr("game://brick.png", 0, popup->ID));
    h.Frames(3);
    REQUIRE(h.editableEdits.size() == 1);                 // once
    CHECK(h.editableEdits[0].op == AssetRefEdit::Op::Set);
    CHECK(h.editableEdits[0].guid == fx.gBrick);

    h.activate = CellHarness::CellItem("Material", ICON_LC_CHEVRON_DOWN "##pick");
    h.Frames(3);
    popup = PopupWindow(CellHarness::CellItem("Material", "##assetpick"));
    REQUIRE(popup != nullptr);
    h.activate = ImHashStr("(none)##assetnone", 0, popup->ID);
    h.Frames(3);
    REQUIRE(h.editableEdits.size() == 2);
    CHECK(h.editableEdits[1].op == AssetRefEdit::Op::Clear);
}

TEST_CASE("AssetReferenceValue: a pill-bearing picker row pitches TableRowHeight() + ItemSpacing.y at a non-default editor.ui.tableRowHeight", "[editor][assetref][ui-style]")
{
    // Settings S6-28 fix round 1: RowWithThumb draws at TableRowHeight(), so
    // the picker's pill-row cursor jump must read it too; a literal 24 made
    // the next row overlap the pill row at any other pitch.
    using Arcane::CVarRegistry;
    struct Revert
    {
        ~Revert()
        {
            CVarRegistry& reg = CVarRegistry::Get();
            reg.RevertLayer(Arcane::SetBy::EditorUser);
            reg.PublishImmediate();
        }
    } revert;
    {
        CVarRegistry& reg = CVarRegistry::Get();
        REQUIRE(reg.Set(reg.Find("editor.ui.tableRowHeight"), Arcane::CVarValue::Float32(30.0f), Arcane::SetBy::EditorUser,
                        "editor", Arcane::CVarContext::Editor) == Arcane::SetResult::Applied);
        reg.PublishImmediate();
    }
    const Ui::ScopedMetrics at1(Ui::Metrics{});
    REQUIRE(TableRowHeight() == 30.0f);

    RefFixture fx("arcane_assetref_pill_pitch_test");
    fx.surfaces[fx.gSubWall] = Arcane::MaterialSurface::Fullscreen;   // sub/wall carries a pill too, and wall follows it
    fx.model.MarkAllDirty();
    REQUIRE(fx.model.RebuildIfDirty(&fx.project->Registry(), fx.providers));
    REQUIRE(fx.model.Find(fx.gSubWall)->surface.has_value());

    CellHarness h(fx);
    h.editable.kindFilter = -1;   // brick (no pill), sub/wall (pill), wall (pill), in that order
    h.Frames(2);
    h.activate = CellHarness::CellItem("Material", ICON_LC_CHEVRON_DOWN "##pick");
    h.Frames(3);
    ImGuiWindow* popup = PopupWindow(CellHarness::CellItem("Material", "##assetpick"));
    REQUIRE(popup != nullptr);
    REQUIRE(popup->Active);

    // A row's Selectable rect, read back through nav: focus the row, draw a
    // frame, and NavProcessItem stores its rect in NavRectRel (imgui.cpp).
    const auto rowTop = [&](const char* mountPath)
    {
        const ImGuiID id = ImHashStr("##row", 0, ImHashStr(mountPath, 0, popup->ID));
        ImGui::SetFocusID(id, popup);
        h.Frames(1);
        REQUIRE(ImGui::GetCurrentContext()->NavId == id);
        const ImRect r = popup->NavRectRel[0];
        CHECK(r.GetHeight() == TableRowHeight());   // the row draws at the setting
        return r.Min.y;
    };
    const float brick = rowTop("game://brick.png");
    const float subWall = rowTop("game://sub/wall.arcmat");
    const float wall = rowTop("game://wall.arcmat");
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    INFO("pill-less pitch " << (subWall - brick) << ", pill pitch " << (wall - subWall));
    CHECK(wall - subWall == TableRowHeight() + spacing);   // the pill row's jump (the old literal gave 24 + spacing)
    // A pill-less row pitches TableRowHeight() alone: RowWithThumb parks the
    // cursor at its own bottom. Pinned so the pre-existing ItemSpacing.y
    // difference between the two stays visible (fix round 1 report).
    CHECK(subWall - brick == TableRowHeight());
}

TEST_CASE("AssetReferenceValue: clear returns Clear once; a read-only cell submits neither chevron nor clear", "[editor][assetref]")
{
    RefFixture fx("arcane_assetref_cell_readonly_test");
    CellHarness h(fx);
    h.editable.guid = fx.gBrick;
    h.Frames(2);
    CHECK(h.editableLog.find(ICON_LC_CHEVRON_DOWN) != std::string::npos);   // the control: an editable cell draws both
    CHECK(h.editableLog.find(ICON_LC_X) != std::string::npos);
    CHECK(h.lockedLog.find(ICON_LC_CHEVRON_DOWN) == std::string::npos);
    CHECK(h.lockedLog.find(ICON_LC_X) == std::string::npos);
    CHECK(h.lockedLog.find(ICON_LC_LOCATE) != std::string::npos);           // navigation is not editing
    h.activate = CellHarness::CellItem("Material", ICON_LC_X "##clear");
    h.Frames(3);
    REQUIRE(h.editableEdits.size() == 1);
    CHECK(h.editableEdits[0].op == AssetRefEdit::Op::Clear);
    h.activate = CellHarness::CellItem("Locked", ICON_LC_CHEVRON_DOWN "##pick");   // never submitted: nothing to press
    h.Frames(3);
    CHECK(PopupWindow(CellHarness::CellItem("Locked", "##assetpick")) == nullptr);
    CHECK(h.lockedEdits.empty());
}

TEST_CASE("AssetReferenceValue: a dangling reference reads \"(missing)\", offers clear, never browse-to", "[editor][assetref]")
{
    RefFixture fx("arcane_assetref_cell_dangling_test");
    fx.services.tombstoneName = [](const Arcane::Guid&) -> std::optional<std::string> { return std::string("old_brick.png"); };
    CellHarness h(fx);
    h.editable.guid = *Arcane::Guid::FromString("deadbeef-0000-4000-8000-000000000001");
    h.Frames(2);
    CHECK(h.editableLog.find("old_brick.png (missing)") != std::string::npos);   // short enough to stay whole at 392 px
    CHECK(h.editableLog.find(ICON_LC_LOCATE) == std::string::npos);            // not browsable
    CHECK(h.editableLog.find(ICON_LC_X) != std::string::npos);                 // but clearable
    h.activate = CellHarness::CellItem("Material", ICON_LC_X "##clear");
    h.Frames(3);
    REQUIRE(h.editableEdits.size() == 1);
    CHECK(h.editableEdits[0].op == AssetRefEdit::Op::Clear);
}

TEST_CASE("AssetReferenceValue: a name double-click opens once; browse-to reveals, disabled while the browser is closed", "[editor][assetref]")
{
    RefFixture fx("arcane_assetref_cell_open_test");
    CellHarness h(fx);
    h.editable.guid = fx.gBrick;
    h.Frames(2);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(h.cellMinX + 40.0f, h.editableTop + ImGui::GetFrameHeight() * 0.5f);   // past the 20 px thumb: the name
    h.Frame();
    io.AddMouseButtonEvent(0, true); h.Frame();
    io.AddMouseButtonEvent(0, false); h.Frame();
    CHECK(h.opens == 0);                                  // a single click does nothing
    io.AddMouseButtonEvent(0, true); h.Frame();
    io.AddMouseButtonEvent(0, false); h.Frame();
    CHECK(h.opens == 1);
    CHECK(h.editableEdits.empty());

    h.activate = CellHarness::CellItem("Material", ICON_LC_LOCATE "##reveal");
    h.Frames(3);
    CHECK(h.reveals == 1);
    h.browserOpen = false;
    h.activate = CellHarness::CellItem("Material", ICON_LC_LOCATE "##reveal");
    h.Frames(3);
    CHECK(h.reveals == 1);                                // disabled: "the Asset Browser is closed"
}

TEST_CASE("AssetReferenceValue: a mixed selection reads \"--\" and offers clear even when the primary is nil", "[editor][assetref]")
{
    RefFixture fx("arcane_assetref_cell_mixed_test");
    CellHarness h(fx);
    h.editable.mixed = true;                              // guid stays nil: the primary has none, others do
    h.Frames(2);
    CHECK(h.editableLog.find("--") != std::string::npos);
    CHECK(h.editableLog.find(ICON_LC_X) != std::string::npos);
    CHECK(h.editableLog.find(ICON_LC_LOCATE) == std::string::npos);
    h.activate = CellHarness::CellItem("Material", ICON_LC_X "##clear");
    h.Frames(3);
    REQUIRE(h.editableEdits.size() == 1);
    CHECK(h.editableEdits[0].op == AssetRefEdit::Op::Clear);
}

TEST_CASE("AssetReferenceValue: a harness drag sets an editable cell and is refused by a read-only one", "[editor][assetref]")
{
    RefFixture fx("arcane_assetref_cell_drop_test");
    CellHarness h(fx);
    h.Frames(2);
    const float mid = ImGui::GetFrameHeight() * 0.5f;
    h.drag = AssetDragPayload{ fx.gBrick, AssetKind::Texture };
    h.DragTo(ImVec2(h.cellMinX + 60.0f, h.editableTop + mid));
    REQUIRE(h.editableEdits.size() == 1);                 // the harness delivers (BeginDragDropSource -> AcceptDragDropPayload)
    CHECK(h.editableEdits[0].op == AssetRefEdit::Op::Set);
    CHECK(h.editableEdits[0].guid == fx.gBrick);
    h.drag = AssetDragPayload{ fx.gBrick, AssetKind::Texture };
    h.DragTo(ImVec2(h.cellMinX + 60.0f, h.lockedTop + mid));
    CHECK(h.lockedEdits.empty());                         // a read-only cell opens no drop target
}

TEST_CASE("AssetReferenceValue: null services read \"No project open\" and still take a Set drop", "[editor][assetref]")
{
    RefFixture fx("arcane_assetref_cell_null_test");
    CellHarness h(fx);
    const AssetRefServices none{};
    h.services = &none;
    h.editable.guid = fx.gBrick;
    h.Frames(2);
    CHECK(h.editableLog.find(fx.gBrick.ToString()) != std::string::npos);   // raw guid (may be ellipsized: the head shows)
    CHECK(h.editableLog.find(ICON_LC_LOCATE) == std::string::npos);         // no browse-to
    const float mid = ImGui::GetFrameHeight() * 0.5f;
    h.drag = AssetDragPayload{ fx.gBrick, AssetKind::Texture };
    h.DragTo(ImVec2(h.cellMinX + 60.0f, h.editableTop + mid));
    REQUIRE(h.editableEdits.size() == 1);                 // spec s4.2: Set drops still work under null services
    CHECK(h.editableEdits[0].op == AssetRefEdit::Op::Set);
    CHECK(h.editableEdits[0].guid == fx.gBrick);
    h.activate = CellHarness::CellItem("Material", ICON_LC_CHEVRON_DOWN "##pick");
    h.Frames(3);
    CHECK(h.editableLog.find("No project open") != std::string::npos);
}

TEST_CASE("AssetRefRow: label cell + cell inside a PropertyGrid, probed under its label", "[editor][assetref]")
{
    RefFixture fx("arcane_assetref_row_test");
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 1024.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    PropertyGridState state;
    std::unordered_map<std::string, ImVec2> probe;
    state.probe = &probe;
    AssetRefArgs args;
    args.guid = fx.gBrick;
    for (int frame = 0; frame < 2; ++frame)
    {
        io.DeltaTime = 1.0f / 60.0f;
        probe.clear();
        ImGui::NewFrame();
        ImGui::Begin("Inspector");
        PropertyGrid grid(state);
        {
            PropertyGrid::Rows rows(grid, "##fields");
            if (rows) (void)AssetRefRow(grid, "Texture", args, fx.services);
        }
        ImGui::End();
        ImGui::Render();
    }
    CHECK(probe.count("Texture") == 1);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

// ---- Part 3: the entity page's AssetRef arm (EditorInspectorVectorTest.cpp shape) ----
namespace
{
    struct EntityRefHarness
    {
        RefFixture fx{ "arcane_assetref_entity_test" };
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };
        Astra::Entity e{};
        Arcane::CommandStack undo{ [this]() -> Astra::Registry& { return reg; } };
        Arcane::Editor::InspectorState state;
        Arcane::Editor::InspectorServices inspectorServices;
        std::unordered_map<std::string, glm::vec2> probe;
        std::vector<Astra::Entity> selection;
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        ImGuiID activate = 0, pickId = 0, popupId = 0;    // the materialOverride cell's chevron + popup
        int materialOverrideHash = 0;
        std::optional<AssetDragPayload> drag;
        ImVec2 srcCentre{};

        EntityRefHarness()
        {
            Arcane::Runtime pin(Arcane::Test::Process());  // shared TypeContext BEFORE registration
            Arcane::RegisterSceneComponents(reg);
            e = reg.CreateEntity();
            reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{});
            Arcane::Identity ident;
            ident.id = Arcane::Guid::Generate();
            ident.name = "E";
            reg.AddComponent<Arcane::Identity>(e, ident);
            selection = { e };
            for (const Astra::FieldInfo& f : Astra::GetMeta<Arcane::MeshRenderer>()->fields)
                if (f.name == "materialOverride") materialOverrideHash = static_cast<int>(f.nameHash);
            inspectorServices.assetRefs = &fx.services;
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 1024.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            state.vectorProbe = &probe;
        }
        ~EntityRefHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }

        Astra::Registry::ComponentInfo Component(std::string_view typeName)
        {
            for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
                if (ci.meta && ci.meta->typeName == typeName) return ci;
            FAIL("the harness entity carries no " << typeName);
            return {};
        }
        Arcane::Guid MaterialOverride() { return reg.GetComponent<Arcane::MeshRenderer>(e)->materialOverride; }
        Arcane::Guid IdentityId() { return reg.GetComponent<Arcane::Identity>(e)->id; }

        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            if (activate) { ImGui::ActivateItemByID(activate); activate = 0; }
            ImGui::SetNextWindowPos(ImVec2(700.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(200.0f, 100.0f), ImGuiCond_Always);
            ImGui::Begin("src");
            ImGui::Button("drag me");
            srcCentre = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                               (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
            if (drag && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
            {
                ImGui::SetDragDropPayload(kAssetDragType, &*drag, sizeof(AssetDragPayload));
                ImGui::EndDragDropSource();
            }
            ImGui::End();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(640.0f, 600.0f), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            {
                Arcane::Editor::FieldGrid grid("##fields", state.grid.labelColWidth);
                if (grid)
                {
                    const Astra::Registry::ComponentInfo mesh = Component("Arcane::MeshRenderer");
                    Arcane::Editor::DrawReflectedComponent({ reg, mesh, e, std::span<const Astra::Entity>(selection), &undo,
                        &*fx.project, &inspectorServices, state, "Mesh Renderer", "Appearance", std::string_view{} });
                    const Astra::Registry::ComponentInfo ident = Component("Arcane::Identity");
                    Arcane::Editor::DrawReflectedComponent({ reg, ident, e, std::span<const Astra::Entity>(selection), &undo,
                        &*fx.project, &inspectorServices, state, "Identity", std::string_view{}, std::string_view{} });
                }
            }
            // The stack the visitor pushes: the grid's table id (BeginTable ->
            // PushOverrideID, imgui_tables.cpp:462), the field's nameHash, the cell's "##assetref".
            ImGui::PushOverrideID(ImGui::GetID("##fields"));
            ImGui::PushID(materialOverrideHash);
            ImGui::PushID("##assetref");
            pickId = ImGui::GetID(ICON_LC_CHEVRON_DOWN "##pick");
            popupId = ImGui::GetID("##assetpick");
            ImGui::PopID(); ImGui::PopID(); ImGui::PopID();
            ImGui::End();
            ImGui::Render();
        }
        void Frames(int n) { for (int i = 0; i < n; ++i) Frame(); }
        void DragTo(glm::vec2 target)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(srcCentre.x, srcCentre.y); Frame();
            io.AddMouseButtonEvent(0, true); Frame();
            io.AddMousePosEvent(srcCentre.x + 20.0f, srcCentre.y); Frame();
            io.AddMousePosEvent(target.x, target.y); Frame();
            Frame();
            io.AddMouseButtonEvent(0, false); Frame();
            Frame();
            drag.reset();
        }
        glm::vec2 Centre(const std::string& key) { INFO(key); REQUIRE(probe.count(key) == 1); return probe.at(key); }
    };
}

TEST_CASE("Entity page: a MeshRenderer material picked in the cell is ONE undo step; undo restores it", "[editor][inspector][assetref]")
{
    EntityRefHarness h;
    h.Frames(2);                                          // frame 1 seeds the label column
    REQUIRE_FALSE(h.MaterialOverride().IsValid());
    h.activate = h.pickId;
    h.Frames(3);
    ImGuiWindow* popup = PopupWindow(h.popupId);
    REQUIRE(popup != nullptr);
    REQUIRE(popup->Active);
    h.activate = ImHashStr("##row", 0, ImHashStr("game://wall.arcmat", 0, popup->ID));   // Mesh surface: offered
    h.Frames(3);
    CHECK(h.MaterialOverride() == h.fx.gWall);
    REQUIRE(h.undo.CanUndo());
    h.undo.Undo();
    CHECK_FALSE(h.MaterialOverride().IsValid());
    CHECK_FALSE(h.undo.CanUndo());                        // exactly one step
}

TEST_CASE("Entity page: Identity::id refuses a texture drop (the control drop on a material field lands)", "[editor][inspector][assetref]")
{
    EntityRefHarness h;
    h.Frames(2);
    h.drag = AssetDragPayload{ h.fx.gWall, AssetKind::Material };
    h.DragTo(h.Centre("materialOverride.cell"));
    REQUIRE(h.MaterialOverride() == h.fx.gWall);          // the harness delivers
    const Arcane::Guid before = h.IdentityId();
    h.drag = AssetDragPayload{ h.fx.gBrick, AssetKind::Texture };
    h.DragTo(h.Centre("id.cell"));
    CHECK(h.IdentityId() == before);
    h.undo.Undo();                                        // the material drop's step...
    CHECK_FALSE(h.undo.CanUndo());                        // ...is the only one
}
