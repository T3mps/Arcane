// SpriteDocument's undo half (widget-layer Task 7), headless. The ImGui form is
// never drawn -- Draw is the only ImGui method and nothing here calls it, the
// same split ShaderEditorDocumentTest uses. What these drive is the pair the
// EditGesture bracket delegates to: ApplySpriteData (an undo step's re-entry
// point, which must republish to the viewport the way a Save does) and
// PushDataEdit (the before/after step builder, including its no-op guard), plus
// the doc-identity anchor that keeps a step on the SHARED stack safe after the
// document it edited is gone.
//
// The one exception is the Inspector-page case at the bottom (inspector
// filters s6a): a device-less ImGui context (InputActionsDocumentUiTest's
// harness shape) that draws the document and then its page inside an
// "Inspector" window, to prove the form is submitted THERE.
//
// A real CommandStack is safe here: its resolve callback is only consulted by
// the COMPONENT snapshot paths (CommandStack.cpp:35, :49), and a generic Push
// (:84-102) never reaches them -- so no registry mutation, no TypeContext, and
// no bare Arcane::Runtime.

#include <catch2/catch_test_macros.hpp>

#include "Documents/SpriteDocument.hpp"
#include "Widgets/PropertyGrid.hpp"

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName / GetActiveID / ActiveIdWindow

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

using Arcane::Editor::SpriteDocument;

namespace
{
    // A stack over a real (but untouched) registry: the resolver has to return
    // a reference, and handing it a live object beats a dangling one even
    // though a generic-command test never calls it.
    struct UndoFixture
    {
        std::shared_ptr<Astra::ComponentRegistry> creg =
            std::make_shared<Astra::ComponentRegistry>();
        std::unique_ptr<Astra::Registry> reg =
            std::make_unique<Astra::Registry>(creg);
        Arcane::CommandStack stack{ [this]() -> Astra::Registry& { return *reg; } };
    };

    Arcane::SpriteAssetData Fixture()
    {
        Arcane::SpriteAssetData d;
        d.id         = Arcane::Guid::Generate();
        d.name       = "hero";
        d.texture    = Arcane::Guid::Generate();
        d.ppu        = 100.0f;
        d.sourcePos  = {0.0f, 0.0f};
        d.sourceSize = {32.0f, 32.0f};
        d.pivot      = {0.5f, 0.5f};
        return d;
    }

    // The document never touches disk outside Save(), so the path only has to
    // be well-formed (it feeds the title fallback).
    std::filesystem::path FixturePath()
    {
        return std::filesystem::path("sprite_undo_test") / "hero.arcsprite";
    }
}

TEST_CASE("SpriteDocument::ApplySpriteData republishes data, dirt, and the viewport", "[editor][sprite]")
{
    int invalidations = 0;
    Arcane::Guid invalidated = Arcane::Guid::Nil();

    const Arcane::SpriteAssetData before = Fixture();
    SpriteDocument::Services services;
    services.invalidateSprite = [&](const Arcane::Guid& g)
    {
        ++invalidations;
        invalidated = g;
    };
    SpriteDocument doc(services, FixturePath(), before);
    CHECK_FALSE(doc.Dirty());

    Arcane::SpriteAssetData after = before;
    after.ppu   = 64.0f;
    after.pivot = {0.25f, 0.75f};
    doc.ApplySpriteData(after);

    CHECK(doc.Data().ppu == 64.0f);
    CHECK(doc.Data().pivot == after.pivot);
    // An undo moves AWAY from the saved bytes, so it dirties the document --
    // and it must invalidate the sprite cache the same way Save does, or the
    // viewport keeps drawing the pre-undo geometry (SpriteCache::Request is a
    // once-per-Guid cache).
    CHECK(doc.Dirty());
    CHECK(invalidations == 1);
    CHECK(invalidated == before.id);
}

TEST_CASE("SpriteDocument edits round-trip through the shared CommandStack", "[editor][sprite]")
{
    UndoFixture fx;
    const Arcane::SpriteAssetData before = Fixture();

    SpriteDocument::Services services;
    services.undo = &fx.stack;
    SpriteDocument doc(services, FixturePath(), before);

    // What a completed drag does: the live edit already happened, then the
    // gesture's close builds one step from the activation-time copy.
    Arcane::SpriteAssetData after = before;
    after.sourceSize = {48.0f, 24.0f};
    doc.ApplySpriteData(after);
    doc.PushDataEdit("Edit Source Size", before);

    REQUIRE(fx.stack.CanUndo());
    CHECK(std::string(fx.stack.UndoLabel()) == "Edit Source Size");

    fx.stack.Undo();
    CHECK(doc.Data() == before);
    REQUIRE(fx.stack.CanRedo());

    fx.stack.Redo();
    CHECK(doc.Data() == after);
}

TEST_CASE("SpriteDocument: a gesture that moved nothing pushes no step", "[editor][sprite]")
{
    UndoFixture fx;
    const Arcane::SpriteAssetData data = Fixture();

    SpriteDocument::Services services;
    services.undo = &fx.stack;
    SpriteDocument doc(services, FixturePath(), data);

    // Press-and-release on a drag without moving it: before == after.
    doc.PushDataEdit("Edit Pivot", doc.Data());
    CHECK_FALSE(fx.stack.CanUndo());

    // Contrast, so the guard above is not vacuous: a real change does push.
    Arcane::SpriteAssetData moved = data;
    moved.pivot = {0.0f, 1.0f};
    doc.ApplySpriteData(moved);
    doc.PushDataEdit("Edit Pivot", data);
    CHECK(fx.stack.CanUndo());
}

TEST_CASE("SpriteDocument undo steps go inert once the document closes", "[editor][sprite]")
{
    UndoFixture fx;
    const Arcane::SpriteAssetData before = Fixture();

    {
        SpriteDocument::Services services;
        services.undo = &fx.stack;
        SpriteDocument doc(services, FixturePath(), before);

        Arcane::SpriteAssetData after = before;
        after.ppu = 32.0f;
        doc.ApplySpriteData(after);
        doc.PushDataEdit("Edit Pixels Per Meter", before);
        REQUIRE(fx.stack.CanUndo());
    }   // document destroyed; the step outlives it on the shared stack

    // The anchor expired, so both directions resolve to nothing and step over
    // themselves rather than dereferencing a dead document.
    CHECK_NOTHROW(fx.stack.Undo());
    CHECK_NOTHROW(fx.stack.Redo());
}

// Inspector filters s6a: the document is an Inspector source of kind "sprite"
// whose ONE page is the sprite form. Opened = selected (epoch 1, so the app's
// per-document epoch map -- which starts at 0 -- sees frame 1 as an event); a
// history restore re-selects without a click and so moves no epoch.
TEST_CASE("SpriteDocument is a sprite Inspector source selected at open", "[editor][sprite][inspector]")
{
    SpriteDocument doc(SpriteDocument::Services{}, FixturePath(), Fixture());
    CHECK(doc.Kind() == "sprite");
    CHECK(doc.SelectionKey() == "sprite");
    CHECK(doc.SelectionEpoch() == 1);                  // selected at open
    REQUIRE(doc.Page() != nullptr);
    CHECK(doc.Page()->Breadcrumb().size() == 1);
    CHECK(doc.Page()->Breadcrumb()[0].label == doc.Title());
    CHECK(doc.Page()->Breadcrumb()[0].key == std::optional<std::string>{ "sprite" });
    CHECK(doc.PageFor("sprite") == doc.Page());
    CHECK(doc.RestoreSelection("sprite"));
    CHECK(doc.SelectionEpoch() == 1);                  // a restore is not a click
    CHECK_FALSE(doc.Resolves("material"));
}

namespace
{
    // InputActionsDocumentUiTest.cpp's DocUi shape: own context, software font
    // atlas, the document drawn FIRST (as DocumentHost::DrawAll does), then a
    // pinned "Inspector" window drawing its page (as DrawInspectorWindows does).
    struct SpritePageUi
    {
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        Arcane::Editor::PropertyGridState grid;
        SpriteDocument doc{ SpriteDocument::Services{}, FixturePath(), Fixture() };
        SpritePageUi()
        {
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1600.0f, 900.0f);
            io.IniFilename = nullptr;
            unsigned char* px = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        }
        ~SpritePageUi() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_Always);
            bool close = false;
            doc.Draw(close);
            ImGui::SetNextWindowPos(ImVec2(820, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(700, 880), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            { Arcane::Editor::PropertyGrid g(grid); if (auto* page = doc.Page()) page->Draw(g); }
            ImGui::End();
            ImGui::Render();
        }
        void Move(ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); Frame(); }
        void Button(int b, bool down) { ImGui::GetIO().AddMouseButtonEvent(b, down); Frame(); }
    };
}

// The form moved OUT of the document window: its first widget ("Pixels Per
// Meter") is submitted in the Inspector window. Proven through the ACTIVE id
// after a press on the page's first row -- ImGuiWindow::GetID only hashes a
// label, and LastItemData is restored to the parent's at End()
// (imgui.cpp:8849), so neither alone shows a submission.
TEST_CASE("SpriteDocument's form draws in the Inspector window, not the document's", "[editor][sprite][inspector]")
{
    SpritePageUi h;
    h.Frame();                                         // warm-up: both windows exist
    ImGuiWindow* iw = ImGui::FindWindowByName("Inspector");
    REQUIRE(iw != nullptr);
    const ImVec2 row(iw->ContentRegionRect.Min.x + 10.0f,
                     iw->ContentRegionRect.Min.y + ImGui::GetFrameHeight() * 0.5f);   // the page's first row
    h.Move(row);
    h.Button(ImGuiMouseButton_Left, true);
    CHECK(ImGui::GetActiveID() == iw->GetID("Pixels Per Meter"));
    CHECK(ImGui::GetCurrentContext()->ActiveIdWindow == iw);   // in the Inspector, not the document window
    h.Button(ImGuiMouseButton_Left, false);
}
