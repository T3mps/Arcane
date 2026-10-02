// Node page + editor upgrades, T5 s7.10 (Task T5-B1): the Asset Browser's key
// routing. FoldEntityClipboardShortcuts is header-inline (EditorPanels.hpp),
// so the fold is unit-tested here without EditorPanels.cpp. The ownership
// cases drive the REAL DrawAssetBrowserPanel over a real Arcane::Project
// through device-less ImGui frames (AssetInspectorSourceTest.cpp's shape).
// BrowserHarness is shared by the later T5-B Browser tasks (rename,
// multi-select, Duplicate, Delete), which extend it in place.

#include <catch2/catch_test_macros.hpp>

#include "Documents/DocumentHost.hpp"
#include "Panels/AssetBrowserPanel.hpp"
#include "Panels/AssetPanelModel.hpp"
#include "Panels/EditorPanels.hpp"

#include <Arcane/Project/Project.hpp>

#include <imgui.h>
#include <imgui_internal.h>   // ActivateItemByID, FindWindowByName, GImGui

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>

using namespace Arcane::Editor; namespace fs = std::filesystem;
namespace
{
    // Real project + device-less ImGui (AssetInspectorSourceTest.cpp's FrameContext shape).
    struct BrowserHarness
    {
        fs::path root; std::optional<Arcane::Project> project; AssetPanelModel model; AssetBrowserPanelState state; DocumentHost docs;
        AssetPanelServices services; ImGuiContext* prev = ImGui::GetCurrentContext(); ImGuiContext* ctx = nullptr;
        explicit BrowserHarness(const char* n, int pngs = 1) : root(fs::temp_directory_path() / n)
        {
            std::error_code ec; fs::remove_all(root, ec); REQUIRE(Arcane::Project::Create(root, "B").has_value());
            for (int i = 0; i < pngs; ++i) { char f[16]; std::snprintf(f, 16, "a%02d.png", i); std::ofstream(root / "Content" / f) << "px"; }
            project = Arcane::Project::Open(root); REQUIRE(project); (void)model.RebuildIfDirty(&project->Registry(), {});
            ctx = ImGui::CreateContext(); ImGui::SetCurrentContext(ctx); ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280, 1024); io.IniFilename = nullptr; unsigned char* px; int w, h; io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        }
        ~BrowserHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); std::error_code ec; fs::remove_all(root, ec); }
        AssetPanelActions Frame(bool focus = false, ImGuiID activate = 0)
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f; ImGui::NewFrame(); if (activate) ImGui::ActivateItemByID(activate);
            ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(800, 300)); if (focus) ImGui::SetNextWindowFocus();
            AssetPanelActions a = DrawAssetBrowserPanel(state, model, &*project, docs, services);
            ImGui::Render(); (void)model.RebuildIfDirty(&project->Registry(), {}); return a;
        }
        ImVec2 RowCenter(int i) const   // Rows()[i] under the frozen 24 px header, inside the deepest child of "Asset Browser"
        {
            ImGuiWindow* top = ImGui::FindWindowByName("Asset Browser"); ImGuiWindow* best = nullptr; int bd = 0;
            for (ImGuiWindow* w : GImGui->Windows) { if (w == top || w->RootWindow != top) continue; int d = 0;
                for (ImGuiWindow* p = w; p != top; p = p->ParentWindow) ++d; if (d > bd) { best = w; bd = d; } }
            REQUIRE(best); return ImVec2(best->Pos.x + 60, best->Pos.y + kTableRowHeight * (i + 1.5f));
        }
        void Mods(ImGuiKeyChord m, bool d)
        { if (m & ImGuiMod_Ctrl) ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, d); if (m & ImGuiMod_Shift) ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, d); }
        void Click(int row, ImGuiMouseButton b, ImGuiKeyChord m = 0)
        {
            ImGuiIO& io = ImGui::GetIO(); Mods(m, true); if (m) Frame();   // the mods get their own frame: a key change trickles the mouse move to the press frame, too late to hover an AllowOverlap row
            const ImVec2 p = RowCenter(row); io.AddMousePosEvent(p.x, p.y); Frame();
            io.AddMouseButtonEvent(b, true); Frame(); io.AddMouseButtonEvent(b, false); Frame(); Mods(m, false); Frame();
        }
        AssetPanelActions Key(ImGuiKey k, ImGuiKeyChord m = 0)
        { Mods(m, true); ImGui::GetIO().AddKeyEvent(k, true); auto a = Frame(); ImGui::GetIO().AddKeyEvent(k, false); Mods(m, false); Frame(); return a; }
    };
}
TEST_CASE("FoldEntityClipboardShortcuts folds none while the Browser owns the keys, all four otherwise", "[editor][assetops]")
{
    MenuRequests owned, free;
    FoldEntityClipboardShortcuts(owned, { true, true, true, true }, true);
    FoldEntityClipboardShortcuts(free, { true, true, true, true }, false);
    CHECK_FALSE((owned.cutSelection || owned.copySelection || owned.paste || owned.duplicateSelection));
    CHECK((free.cutSelection && free.copySelection && free.paste && free.duplicateSelection));
}
TEST_CASE("Asset Browser owns the edit keys only when focused, with no popup and no text field", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_keys_test"); (void)h.Frame(true);
    CHECK(h.Frame().ownsEditKeys);
    SECTION("row context menu") { h.Click(1, ImGuiMouseButton_Right); CHECK(h.model.selected.IsValid()); CHECK_FALSE(h.Frame().ownsEditKeys); }
    SECTION("search box")
    { (void)h.Frame(false, ImGui::FindWindowByName("Asset Browser")->GetID("##assetssearch")); (void)h.Frame(); CHECK_FALSE(h.Frame().ownsEditKeys); }
}
TEST_CASE("Asset Browser F2 opens the inline rename; Enter commits a Rename; Esc reverts", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_rename_test"); (void)h.Frame(true);
    const Arcane::Guid g = h.model.Rows()[1].guid; h.model.Select(g); (void)h.Key(ImGuiKey_F2);
    REQUIRE(h.state.renameTarget == g);
    CHECK((std::string(h.state.renameBuf) == "a00" && !h.Frame().ownsEditKeys));   // stem only; the box owns the keys
    SECTION("Enter commits")
    {
        ImGui::GetIO().AddInputCharactersUTF8("wall"); (void)h.Frame();   // typed into the active box (AutoSelectAll replaces "a00")
        const AssetPanelActions a = h.Key(ImGuiKey_Enter);
        REQUIRE(a.fileOp); CHECK((a.fileOp->kind == AssetOpKind::Rename && a.fileOp->newStem == "wall" && !h.state.renameTarget.IsValid()));
    }
    SECTION("Esc reverts") { CHECK_FALSE(h.Key(ImGuiKey_Escape).fileOp); CHECK_FALSE(h.state.renameTarget.IsValid()); }
}
TEST_CASE("Asset Browser Ctrl+D raises a Duplicate and keeps the keys from the entity clipboard", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_dup_test"); (void)h.Frame(true); h.model.Select(h.model.Rows()[1].guid);
    const AssetPanelActions a = h.Key(ImGuiKey_D, ImGuiMod_Ctrl);
    REQUIRE(a.fileOp); CHECK((a.fileOp->kind == AssetOpKind::Duplicate && a.fileOp->guids == std::vector<Arcane::Guid>{ h.model.selected } && a.ownsEditKeys));
}
TEST_CASE("Asset Browser Del requests a delete confirm; with a row menu open Del requests nothing", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_del_test"); (void)h.Frame(true); h.model.Select(h.model.Rows()[1].guid);
    CHECK(h.Key(ImGuiKey_Delete).requestDelete == std::vector<Arcane::Guid>{ h.model.selected });
    h.Click(1, ImGuiMouseButton_Right); CHECK(h.Key(ImGuiKey_Delete).requestDelete.empty());
}
TEST_CASE("Asset Browser multi-select: Ctrl-click then Shift-click over a clipped list; Ctrl+A never selects a group row", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_multiselect_test", 40); (void)h.Frame(true);   // 41 rows in a 300 px window: clipped
    const auto& rows = h.model.Rows(); REQUIRE(rows[0].type == AssetPanelRow::Type::Group);
    h.Click(1, ImGuiMouseButton_Left); h.Click(3, ImGuiMouseButton_Left, ImGuiMod_Ctrl);
    CHECK((h.model.SelectionCount() == 2 && h.model.selected == rows[3].guid));
    h.Click(5, ImGuiMouseButton_Left, ImGuiMod_Shift);   // range from the Ctrl-click source
    CHECK((h.model.SelectionCount() == 3 && h.model.InSelection(rows[4].guid) && !h.model.InSelection(rows[1].guid)));
    (void)h.Key(ImGuiKey_A, ImGuiMod_Ctrl); CHECK(h.model.SelectionCount() == 40);   // the adapter skips the group row
}
TEST_CASE("Asset Browser batch keys: Del and Ctrl+D carry the whole selection; F2 needs exactly one", "[editor][assetops]")
{
    BrowserHarness h("arcane_browser_batch_keys_test", 3); (void)h.Frame(true);
    const auto& rows = h.model.Rows();
    h.Click(1, ImGuiMouseButton_Left); h.Click(2, ImGuiMouseButton_Left, ImGuiMod_Ctrl);
    const std::vector<Arcane::Guid> sel = h.model.selection; REQUIRE(sel.size() == 2);
    CHECK(h.Key(ImGuiKey_Delete).requestDelete == sel);
    const AssetPanelActions d = h.Key(ImGuiKey_D, ImGuiMod_Ctrl);
    REQUIRE(d.fileOp); CHECK((d.fileOp->kind == AssetOpKind::Duplicate && d.fileOp->guids == sel));
    (void)h.Key(ImGuiKey_F2); CHECK_FALSE(h.state.renameTarget.IsValid());
    h.Click(1, ImGuiMouseButton_Right);   // inside the selection: the set stays, the primary moves
    CHECK((h.model.selection == sel && h.model.selected == rows[1].guid));
}
