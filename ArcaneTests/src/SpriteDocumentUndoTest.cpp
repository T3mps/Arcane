// SpriteDocument's undo half (widget-layer Task 7), headless. The undo cases
// draw nothing -- the form is the Inspector's sprite page now (DrawFormBody,
// inspector filters s6a) and these never draw it. What these drive is the pair the
// EditGesture bracket delegates to: ApplySpriteData (an undo step's re-entry
// point, which must republish to the viewport the way a Save does) and
// PushDataEdit (the before/after step builder, including its no-op guard), plus
// the doc-identity anchor that keeps a step on the SHARED stack safe after the
// document it edited is gone.
//
// The exceptions are the ImGui cases at the bottom (inspector filters s6a,
// final fix D): a device-less ImGui context (InputActionsDocumentUiTest's
// harness shape) that draws the document and then its page inside an
// "Inspector" window, to prove the form is submitted THERE.
//
// A real CommandStack is safe here: its resolve callback is only consulted by
// the COMPONENT snapshot paths (CommandStack.cpp:35, :49), and a generic Push
// (:84-102) never reaches them -- so no registry mutation, no TypeContext, and
// no bare Arcane::Runtime.

#include <catch2/catch_test_macros.hpp>

#include "Documents/DocumentHost.hpp"
#include "Helpers/SettingsSweep.hpp"
#include "Documents/SpriteDocument.hpp"
#include "Scene/UndoGate.hpp"
#include "Settings/DocumentSettings.hpp"   // editor.sprite.* (the PPU row bounds)
#include "Widgets/PropertyGrid.hpp"

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Config/CVarRegistry.hpp>   // the editor.sprite.ppuMax Code-rung override
#include <Arcane/Config/Settings.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName / GetActiveID / ActiveIdWindow

#include <glm/glm.hpp>

#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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
    services.undo = [p = &fx.stack]() -> Arcane::CommandStack* { return p; };
    SpriteDocument doc(services, FixturePath(), before);

    // What a completed drag does: the live edit already happened, then the
    // gesture's close builds one step from the activation-time copy.
    Arcane::SpriteAssetData after = before;
    after.sourceSize = {48.0f, 24.0f};
    doc.ApplySpriteData(after);
    doc.PushDataEdit("Edit Source Size", before);

    REQUIRE(fx.stack.CanUndo());
    CHECK(std::string(fx.stack.UndoLabel()) == "Edit Source Size");
    CHECK(fx.stack.SceneStateId() == 0);   // a document step never dirties the scene (s3.3a)

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
    services.undo = [p = &fx.stack]() -> Arcane::CommandStack* { return p; };
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
        services.undo = [p = &fx.stack]() -> Arcane::CommandStack* { return p; };
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

TEST_CASE("SpriteDocument: closing the document expires its steps; they never cost a Ctrl+Z", "[editor][sprite][undo]")
{
    UndoFixture fx;
    const Arcane::SpriteAssetData before = Fixture();
    {
        SpriteDocument::Services services;
        services.undo = [p = &fx.stack]() -> Arcane::CommandStack* { return p; };
        SpriteDocument doc(services, FixturePath(), before);
        Arcane::SpriteAssetData after = before;
        after.sourceSize = {48.0f, 24.0f};
        doc.ApplySpriteData(after);
        doc.PushDataEdit("Edit Source Size", before);
        REQUIRE(fx.stack.CanUndo());
    }   // the document closes: its anchor dies
    CHECK_FALSE(fx.stack.CanUndo());
    CHECK(std::string(fx.stack.UndoLabel()).empty());
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

TEST_CASE("SpriteDocument: opening it again through DocumentHost::OpenPath re-selects its page", "[editor][sprite][inspector]")
{
    // Final fix R: the focus-not-reopen branch bumps the page epoch like a
    // fresh open, so the app's epoch poll routes the Inspector back to it.
    const Arcane::SpriteAssetData data = Fixture();
    Arcane::Editor::DocumentHost host;
    host.RegisterFactory(".arcsprite", [&](const std::filesystem::path& p) -> std::unique_ptr<Arcane::Editor::EditorDocument>
    { return std::make_unique<SpriteDocument>(SpriteDocument::Services{}, p, data); });
    Arcane::Editor::EditorDocument* doc = host.OpenPath(FixturePath());
    REQUIRE(doc != nullptr);
    CHECK(doc->SelectionEpoch() == 1);                 // selected at open
    CHECK(host.OpenPath(FixturePath()) == doc);        // focus-not-reopen
    CHECK(doc->SelectionEpoch() == 2);                 // ...and re-selected
    CHECK(host.OpenPath(FixturePath()) == doc);
    CHECK(doc->SelectionEpoch() == 3);
    CHECK(host.Count() == 1);
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
        std::unordered_map<std::string, ImVec2> probe;   // PropertyGrid's test seam: label -> the row's value centre
        SpriteDocument doc;
        explicit SpritePageUi(SpriteDocument::Services s = {}, Arcane::SpriteAssetData d = Fixture())
            : doc(std::move(s), FixturePath(), std::move(d))
        {
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1600.0f, 900.0f);
            io.IniFilename = nullptr;
            unsigned char* px = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
            grid.probe = &probe;
        }
        ~SpritePageUi() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
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
        void Click(ImVec2 p) { Move(p); Button(0, true); Button(0, false); }
        ImVec2 At(const std::string& label) { INFO(label); REQUIRE(probe.count(label) == 1); return probe.at(label); }
    };

    // An Assets facade that knows one texture size -- TextureInfoFor is all the page reads.
    class DimsAssets final : public Arcane::Assets
    {
    public:
        DimsAssets(std::uint32_t w, std::uint32_t h) { m_info.width = w; m_info.height = h; m_info.mipCount = 1; }
        void SetContentRoot(const std::filesystem::path&) override {}
        void SetAssetResolver(AssetResolver) override {}
        const Arcane::TextureInfo* TextureInfoFor(const Arcane::Guid&) override { return &m_info; }
        const Arcane::PixelData* PixelsFor(const Arcane::Guid&) override { return nullptr; }
        std::shared_ptr<const std::vector<std::uint8_t>> GetBytes(const std::filesystem::path&) override { return nullptr; }
        std::shared_ptr<const std::vector<std::uint8_t>> GetBytes(const Arcane::AssetId&) override { return nullptr; }
        std::shared_ptr<const nlohmann::json> GetJson(const std::filesystem::path&) override { return nullptr; }
        std::shared_ptr<const nlohmann::json> GetJson(const Arcane::AssetId&) override { return nullptr; }
        Arcane::AssetStats Stats() const override { return {}; }
        const Arcane::LoadedClientArtifact* ArtifactFor(const Arcane::Guid&) override { return nullptr; }
        void InvalidateArtifact(const Arcane::Guid&) override {}
        void SetCookPendingProbe(std::function<bool(const Arcane::Guid&)>) override {}
        std::optional<Arcane::MaterialSurface> MaterialSurfaceFor(const Arcane::Guid&) override { return std::nullopt; }
        std::optional<std::vector<Arcane::AssetRef>> ListAssetReferences(const Arcane::Guid&) override { return std::nullopt; }
        const Arcane::LoadedClientMesh* MeshArtifactFor(const Arcane::Guid&) override { return nullptr; }
        void InvalidateMeshArtifact(const Arcane::Guid&) override {}
        bool CookPending(const Arcane::Guid&) const override { return false; }
        // T5 s7.2 (interface-completeness only): the page never evicts or retracts.
        void EvictPath(const std::filesystem::path&) override {}
        void ForgetUnresolved(const Arcane::Guid&) override {}
    private:
        Arcane::TextureInfo m_info;
    };
}

// The form moved OUT of the document window: its first drag ("Pixels Per
// Meter") is submitted in the Inspector window. Proven through the ACTIVE id
// after a press on that row (located by the PropertyGrid probe) --
// ImGuiWindow::GetID only hashes a label, and LastItemData is restored to the
// parent's at End() (imgui.cpp:8849), so neither alone shows a submission.
TEST_CASE("SpriteDocument's form draws in the Inspector window, not the document's", "[editor][sprite][inspector]")
{
    SpritePageUi h;
    h.Frame();                                         // warm-up: both windows exist
    ImGuiWindow* iw = ImGui::FindWindowByName("Inspector");
    REQUIRE(iw != nullptr);
    h.Frame();
    const ImVec2 at = h.At("Pixels Per Meter");
    h.Move(at);
    h.Button(ImGuiMouseButton_Left, true);
    // PropertyGrid ids: PushID(label) + "##value" under the section's Rows table ("##sprite").
    CHECK(ImGui::GetActiveID() == ImGui::GetIDWithSeed("##value", nullptr,
                                  ImGui::GetIDWithSeed("Pixels Per Meter", nullptr, iw->GetID("##sprite"))));
    CHECK(ImGui::GetCurrentContext()->ActiveIdWindow == iw);   // in the Inspector, not the document window
    h.Button(ImGuiMouseButton_Left, false);
}

TEST_CASE("SpriteDocument's window points at the Inspector and draws the sprite through the chrome thumbnail seam", "[editor][sprite][inspector]")
{
    // Final fix D: the old "(no texture)" line read as a fact about the
    // sprite once the form moved out. The window now says where the
    // properties are, names the texture, and draws it (cropped to the
    // sprite's rect when the texture's dims are known).
    SpriteDocument::Services services;
    std::vector<Arcane::Guid> asked;
    services.resolveThumb = [&](const Arcane::Guid& g) -> std::uint64_t { asked.push_back(g); return 0xBEEF; };
    services.assetName = [](const Arcane::Guid&) { return std::string("hero_sheet.png"); };
    const Arcane::SpriteAssetData data = Fixture();
    SpriteDocument doc(services, FixturePath(), data);

    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* px = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    std::string logged;
    bool drewThumb = false;
    for (int frame = 0; frame < 2; ++frame)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_Always);
        if (frame == 1) ImGui::LogToBuffer();
        bool close = false;
        doc.Draw(close);
        if (frame == 1) { logged = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
        ImGui::Render();
        if (frame == 1)
            for (int l = 0; l < ImGui::GetDrawData()->CmdListsCount; ++l)
                for (const ImDrawCmd& cmd : ImGui::GetDrawData()->CmdLists[l]->CmdBuffer)
                    if (cmd.TexRef._TexData == nullptr && cmd.TexRef._TexID == static_cast<ImTextureID>(0xBEEF)) drewThumb = true;   // a user texture (the font atlas is a TexData ref)
    }
    INFO(logged);
    CHECK(logged.find("Sprite properties are in the Inspector") != std::string::npos);
    CHECK(logged.find("hero_sheet.png") != std::string::npos);
    CHECK(logged.find("(no texture)") == std::string::npos);
    CHECK(drewThumb);
    REQUIRE_FALSE(asked.empty());
    CHECK(asked.back() == data.texture);               // the sprite's TEXTURE through the seam
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

TEST_CASE("SpriteDocument: the undo resolver is asked per edit -- null in Play pushes nothing", "[editor][sprite][undo]")
{
    UndoFixture fx;
    const Arcane::SpriteAssetData before = Fixture();
    bool playing = true;
    SpriteDocument::Services services;
    services.undo = [&]() { return Arcane::Editor::ResolveDocumentUndo(playing, &fx.stack); };
    SpriteDocument doc(services, FixturePath(), before);

    Arcane::SpriteAssetData after = before;
    after.sourceSize = {48.0f, 24.0f};
    doc.ApplySpriteData(after);
    doc.PushDataEdit("Edit Source Size", before);
    CHECK(doc.Data() == after);                 // the edit stands...
    CHECK_FALSE(fx.stack.CanUndo());            // ...with no step in Play

    playing = false;                            // Stop
    Arcane::SpriteAssetData again = after;
    again.sourceSize = {64.0f, 24.0f};
    doc.ApplySpriteData(again);
    doc.PushDataEdit("Edit Source Size", after);
    CHECK(fx.stack.CanUndo());
}

TEST_CASE("SpriteDocument::SetWholeTexture: ticked is (0,0); unticked is the texture's size; unknown dims refuse", "[editor][sprite]")
{
    Arcane::SpriteAssetData d = Fixture();
    REQUIRE(SpriteDocument::SetWholeTexture(d, true, 0, 0));
    CHECK(d.sourcePos == glm::vec2(0.0f)); CHECK(d.sourceSize == glm::vec2(0.0f));
    CHECK_FALSE(SpriteDocument::SetWholeTexture(d, false, 0, 0));      // dims unknown: untouched
    CHECK(d.sourceSize == glm::vec2(0.0f));
    REQUIRE(SpriteDocument::SetWholeTexture(d, false, 256, 128));
    CHECK(d.sourcePos == glm::vec2(0.0f)); CHECK(d.sourceSize == glm::vec2(256.0f, 128.0f));
    CHECK(SpriteDocument::TextureRefArgs(d).readOnly);                 // the Texture row: no picker, clear or drop
    CHECK(SpriteDocument::TextureRefArgs(d).guid == d.texture);
}

TEST_CASE("SpriteDocument page: each Whole texture flip is one step and round-trips (0,0)", "[editor][sprite][inspector]")
{
    UndoFixture fx;
    DimsAssets dims(256, 128);
    SpriteDocument::Services s;
    s.undo = [&fx] { return &fx.stack; };
    s.assets = &dims;
    SpritePageUi h(s);                                                   // Fixture: a (32, 32) sub-rect
    h.Frame(); h.Frame();
    h.Click(h.At("Whole texture"));
    CHECK(h.doc.Data().sourceSize == glm::vec2(0.0f));
    REQUIRE(fx.stack.CanUndo());
    CHECK(std::string(fx.stack.UndoLabel()) == "Whole Texture");
    h.Click(h.At("Whole texture"));
    CHECK(h.doc.Data().sourceSize == glm::vec2(256.0f, 128.0f));
    fx.stack.Undo();
    CHECK(h.doc.Data().sourceSize == glm::vec2(0.0f));
    fx.stack.Undo();
    CHECK(h.doc.Data().sourceSize == glm::vec2(32.0f, 32.0f));
    CHECK_FALSE(fx.stack.CanUndo());
}

TEST_CASE("SpriteDocument page: with the texture size unknown the ticked box stays disabled", "[editor][sprite][inspector]")
{
    UndoFixture fx;
    SpriteDocument::Services s;
    s.undo = [&fx] { return &fx.stack; };                              // no Assets: dims unknown
    Arcane::SpriteAssetData d = Fixture();
    d.sourceSize = { 0.0f, 0.0f };
    SpritePageUi h(s, d);
    h.Frame(); h.Frame();
    h.Click(h.At("Whole texture"));
    CHECK(h.doc.Data().sourceSize == glm::vec2(0.0f));
    CHECK_FALSE(fx.stack.CanUndo());
}

TEST_CASE("SpriteDocument page: a Pixels Per Meter drag is one step", "[editor][sprite][inspector]")
{
    UndoFixture fx;
    SpriteDocument::Services s;
    s.undo = [&fx] { return &fx.stack; };
    SpritePageUi h(s);
    h.Frame(); h.Frame();
    const ImVec2 at = h.At("Pixels Per Meter");
    h.Move(at); h.Button(0, true);
    h.Move(ImVec2(at.x + 40.0f, at.y));
    h.Button(0, false); h.Frame();
    CHECK(h.doc.Data().ppu != 100.0f);
    REQUIRE(fx.stack.CanUndo());
    CHECK(std::string(fx.stack.UndoLabel()) == "Edit Pixels Per Meter");
    fx.stack.Undo();
    CHECK(h.doc.Data().ppu == 100.0f);
    CHECK_FALSE(fx.stack.CanUndo());
}

// S6-5 fix round 1: a sprite seeded above the 4096 cap (assets.sprite.
// defaultPixelsPerUnit reaches 10000) is re-editable in place. Under a plain
// Range(1, 4096) a leftward drag from 5000 snaps to 4096 (ImGui clamps a moved
// value to max); since S6-42 fix round 1 the row's editor.sprite.ppuMin/ppuMax
// widen to include the current value, so it moves by the drag alone.
TEST_CASE("SpriteDocument page: a Pixels Per Meter above 4096 drags without snapping to a cap", "[editor][sprite][inspector]")
{
    UndoFixture fx;
    SpriteDocument::Services s;
    s.undo = [&fx] { return &fx.stack; };
    Arcane::SpriteAssetData d = Fixture();
    d.ppu = 5000.0f;
    SpritePageUi h(s, d);
    h.Frame(); h.Frame();
    const ImVec2 at = h.At("Pixels Per Meter");
    h.Move(at); h.Button(0, true);
    h.Move(ImVec2(at.x - 40.0f, at.y));
    h.Button(0, false); h.Frame();
    CHECK(h.doc.Data().ppu < 5000.0f);   // the drag moved it
    CHECK(h.doc.Data().ppu > 4096.0f);   // and no 4096 cap caught it
    REQUIRE(fx.stack.CanUndo());
    fx.stack.Undo();
    CHECK(h.doc.Data().ppu == 5000.0f);
}

namespace
{
    // A Code-rung override on a Live editor setting, removed again however
    // the case exits; PublishImmediate makes Settings<T>() see it this frame.
    struct SpriteCodeOverride
    {
        SpriteCodeOverride(const char* name, const Arcane::CVarValue& value)
            : handle(Arcane::CVarRegistry::Get().Find(name))
        {
            INFO("cvar " << name);
            REQUIRE_FALSE(handle.IsStale());
            REQUIRE(Arcane::CVarRegistry::Get().Set(handle, value, Arcane::SetBy::Code) == Arcane::SetResult::Applied);
            Arcane::CVarRegistry::Get().PublishImmediate();
        }
        ~SpriteCodeOverride()
        {
            Arcane::CVarRegistry::Get().ClearRung(handle, Arcane::SetBy::Code);
            Arcane::CVarRegistry::Get().PublishImmediate();
        }
        SpriteCodeOverride(const SpriteCodeOverride&) = delete;
        SpriteCodeOverride& operator=(const SpriteCodeOverride&) = delete;

        Arcane::CVarHandle handle;
    };

    // Presses the Pixels Per Meter row and drags it 300 px right: at the
    // default 0.5 drag speed that asks for +150, past either cap below.
    void DragPpuRight(SpritePageUi& h)
    {
        h.Frame(); h.Frame();
        const ImVec2 at = h.At("Pixels Per Meter");
        h.Move(at); h.Button(0, true);
        h.Move(ImVec2(at.x + 300.0f, at.y));
        h.Button(0, false); h.Frame();
    }
}

// S6-42 fix round 1: editor.sprite.ppuMax is Live and bounds the row -- a
// changed preference changes the editable limit (spec s3: a Live setting takes
// effect at the next Publish). The seed setting's declared [1, 10000] no longer
// widens it.
TEST_CASE("SpriteDocument page: editor.sprite.ppuMax caps a Pixels Per Meter drag", "[editor][sprite][inspector]")
{
    REQUIRE(Arcane::Settings<Arcane::Editor::SpriteDocSettings>().ppuDragSpeed == 0.5f);   // the +150 arithmetic below

    SECTION("a non-default ppuMax (200) caps a rightward drag from 100")
    {
        if (!Arcane::Test::InThisBuild("editor.sprite.ppuMax")) return;   // Dev: compiled out of Dist
        const SpriteCodeOverride cap("editor.sprite.ppuMax", Arcane::CVarValue::Float32(200.0f));
        REQUIRE(Arcane::Settings<Arcane::Editor::SpriteDocSettings>().ppuMax == 200.0f);
        UndoFixture fx;
        SpriteDocument::Services s;
        s.undo = [&fx] { return &fx.stack; };
        SpritePageUi h(s);                       // Fixture(): ppu 100
        DragPpuRight(h);
        CHECK(h.doc.Data().ppu > 100.0f);        // the drag moved it
        CHECK(h.doc.Data().ppu <= 200.0f);       // and the preference caught it (unclamped: 250)
        REQUIRE(fx.stack.CanUndo());
        fx.stack.Undo();
        CHECK(h.doc.Data().ppu == 100.0f);
    }
    SECTION("the default ppuMax (4096) caps a rightward drag from 4000")
    {
        REQUIRE(Arcane::Settings<Arcane::Editor::SpriteDocSettings>().ppuMax == 4096.0f);
        UndoFixture fx;
        SpriteDocument::Services s;
        s.undo = [&fx] { return &fx.stack; };
        Arcane::SpriteAssetData d = Fixture();
        d.ppu = 4000.0f;
        SpritePageUi h(s, d);
        DragPpuRight(h);
        CHECK(h.doc.Data().ppu > 4000.0f);
        CHECK(h.doc.Data().ppu <= 4096.0f);      // not the seed setting's 10000 (unclamped: 4150)
    }
}

// The grouped VecRow bracket (closed by EndAfterRow) is its own path, apart
// from the FloatRow one above: a Source Size drag is one step too (s5.4
// "each drag is one step"). A 2-box group's probed centre is the 0|1 gap, so
// the press lands 20 px left of it, inside box 0 (sourceSize.x).
TEST_CASE("SpriteDocument page: a Source Size drag is one whole-pixel step", "[editor][sprite][inspector]")
{
    UndoFixture fx;
    SpriteDocument::Services s;
    s.undo = [&fx] { return &fx.stack; };
    SpritePageUi h(s);                                                   // Fixture: a (32, 32) sub-rect
    h.Frame(); h.Frame();
    const ImVec2 c = h.At("Source Size");
    const ImVec2 at(c.x - 20.0f, c.y);
    h.Move(at); h.Button(0, true);
    h.Move(ImVec2(at.x + 40.0f, at.y));
    h.Button(0, false); h.Frame();
    const glm::vec2 dragged = h.doc.Data().sourceSize;
    CHECK(dragged.x != 32.0f);
    CHECK(dragged.x == std::floor(dragged.x));                          // "%.0f": a whole number of pixels
    CHECK(dragged.y == 32.0f);
    REQUIRE(fx.stack.CanUndo());
    CHECK(std::string(fx.stack.UndoLabel()) == "Edit Source Size");
    fx.stack.Undo();
    CHECK(h.doc.Data().sourceSize == glm::vec2(32.0f, 32.0f));
    CHECK_FALSE(fx.stack.CanUndo());
}
