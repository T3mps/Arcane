// Device-less ImGui tests for the Input Actions document (the PropertyGridTest
// harness shape: own context, software font atlas, windows pinned, events
// injected between frames). Covers what only ImGui can observe: the page's
// Rebind focus (final review I1), keys idle under a context menu (I2), the
// page's validated Name row (arc-1 debt B).
#include <catch2/catch_test_macros.hpp>
#include "Documents/InputActionsDocument.hpp"
#include "Documents/InputActionsDocumentWidgets.hpp"
#include "Documents/InputActionsEditorModel.hpp"
#include "Widgets/PropertyGrid.hpp"
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>

namespace
{
    namespace fs = std::filesystem;
    using Arcane::Guid;
    Guid G(const char* s) { return *Guid::FromString(s); }
    const char* kDoc = R"JSON({
        "version":1,"id":"11111111-1111-4111-8111-111111111111","defaultMap":"22222222-2222-4222-8222-222222222222","controlSchemes":[],
        "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"Player","actions":[
            {"id":"33333333-3333-4333-8333-333333333333","name":"Jump","type":"Button","bindings":[{"id":"44444444-4444-4444-8444-444444444444","path":"<Keyboard>/space"}]},
            {"id":"66666666-6666-4666-8666-666666666666","name":"Crouch","type":"Button","bindings":[{"id":"77777777-7777-4777-8777-777777777777","path":"<Keyboard>/c"}]}]},
          {"id":"55555555-5555-4555-8555-555555555555","name":"UI","actions":[]}]})JSON";

    struct Ui
    {
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        std::unordered_map<std::string, ImVec2> probe;
        Ui()
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
        ~Ui() { if (ctx) { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); } }   // DestroyContext(nullptr) would destroy the CURRENT context
        ImVec2 At(const std::string& key) { INFO(key); REQUIRE(probe.count(key) == 1); return probe.at(key); }
        static bool AnyPopup() { return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel); }
    };

    // The document drawn FIRST (as DocumentHost::DrawAll does), then a plain
    // "Inspector" window drawing its page (as DrawInspectorWindows does).
    struct DocUi : Ui
    {
        fs::path path;
        std::unique_ptr<Arcane::Editor::InputActionsDocument> doc;
        Arcane::Editor::PropertyGridState grid;
        DocUi()
        {
            path = fs::temp_directory_path() / ("ui-" + Guid::Generate().ToString() + ".arcinput");
            { std::ofstream out(path); out << kDoc; }
            doc = Arcane::Editor::InputActionsDocument::Open(path);
            grid.probe = &probe;
        }
        bool collapseDoc = false;   // true = the document window draws collapsed: its body is not drawn (Begin returns false)
        // ~Ui owns the context teardown: the document's destructor touches no
        // ImGui (Diagnostics::Clear only). The remove must not throw from a
        // destructor (a briefly locked temp file would std::terminate the run).
        ~DocUi() { doc.reset(); std::error_code ec; fs::remove(path, ec); }
        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            Arcane::Editor::PropertyGrid(grid).CommitOrphans();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_Always);
            if (collapseDoc) ImGui::SetNextWindowCollapsed(true, ImGuiCond_Always);
            bool close = false;
            doc->Draw(close);
            ImGui::SetNextWindowPos(ImVec2(820, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(700, 880), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            { Arcane::Editor::PropertyGrid g(grid); if (auto* page = doc->Page()) page->Draw(g); }
            ImGui::End();
            ImGui::Render();
        }
        void Move(ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); Frame(); }
        void Button(int b, bool down) { ImGui::GetIO().AddMouseButtonEvent(b, down); Frame(); }
        void Key(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, true); Frame(); ImGui::GetIO().AddKeyEvent(k, false); Frame(); }
        void Type(const char* s) { ImGui::GetIO().AddInputCharactersUTF8(s); Frame(); }
    };
}

TEST_CASE("input document: the Inspector page's Rebind... takes focus, so the capture survives the frames after the click; Escape still cancels (final review I1)", "[editor][input][inspector]")
{
    DocUi ui;
    REQUIRE(ui.doc);
    REQUIRE(ui.doc->SelectByPath("Player/Jump/0"));
    ui.Frame(); ui.Frame();
    const ImVec2 rebind = ui.At("#Rebind...");   // ButtonRow's "" label + "#" + the button text
    CHECK_FALSE(ui.doc->InputSwallowed());
    ui.Move(rebind);
    ui.Button(0, true);                          // press: the Inspector takes focus
    ui.Button(0, false);                         // release: the button fires AFTER the document drew
    CHECK(ui.doc->InputSwallowed());
    CHECK(ui.doc->State().scrollRowToId == G("44444444-4444-4444-8444-444444444444"));   // set during the Inspector's draw, consumed next frame
    ui.Frame(); ui.Frame();
    CHECK(ui.doc->WindowFocused());              // SetNextWindowFocus pulled the document forward
    CHECK(ui.doc->InputSwallowed());             // pre-fix the capture cancelled on the first frame
    CHECK_FALSE(ui.doc->State().scrollRowToId.IsValid());
    ui.Key(ImGuiKey_Escape);
    CHECK_FALSE(ui.doc->InputSwallowed());
}

TEST_CASE("input document: a page Rebind cancelled on a frame the document body is not drawn drops its scroll-to-row one-shot (final review)", "[editor][input][inspector]")
{
    DocUi ui;
    REQUIRE(ui.doc);
    REQUIRE(ui.doc->SelectByPath("Player/Jump/0"));
    ui.Frame(); ui.Frame();
    ui.Move(ui.At("#Rebind..."));
    ui.Button(0, true);
    ui.Button(0, false);                         // the page arms the capture and sets the one-shot
    REQUIRE(ui.doc->State().scrollRowToId == G("44444444-4444-4444-8444-444444444444"));
    ui.collapseDoc = true;                       // next frame the body is not drawn: TickCapture(false) cancels, DrawActions never runs
    ui.Frame();                                  // the cancelling frame (still swallowed: the stamp covers it by design)
    ui.Frame();
    CHECK_FALSE(ui.doc->InputSwallowed());       // the capture is over
    CHECK_FALSE(ui.doc->State().scrollRowToId.IsValid());   // pre-fix it lingered and scrolled the row a frame later with no capture live
}

TEST_CASE("input document: the Inspector page's Name row keeps a refused duplicate on Enter with no edit; a unique name commits", "[editor][input][inspector]")
{
    DocUi ui;
    REQUIRE(ui.doc);
    REQUIRE(ui.doc->SelectByPath("Player/Crouch"));
    ui.Frame(); ui.Frame();
    ui.Move(ui.At("Name")); ui.Button(0, true); ui.Button(0, false);
    ui.Type("Jump");
    ui.Key(ImGuiKey_Enter);
    ui.Frame(); ui.Frame();
    const auto* crouch = ui.doc->Model().FindNode(G("66666666-6666-4666-8666-666666666666"));
    REQUIRE(crouch);
    CHECK((*crouch)["name"] == "Crouch");                        // no edit, no undo entry
    bool kept = false;
    for (const auto& [id, d] : ui.grid.textDrafts) kept = kept || d.text == "Jump";
    CHECK(kept);
    ui.Type("Duck");
    ui.Move(ImVec2(1200, 850)); ui.Button(0, true); ui.Button(0, false);
    ui.Frame();
    CHECK((*ui.doc->Model().FindNode(G("66666666-6666-4666-8666-666666666666")))["name"] == "Duck");
}

namespace
{
    // The widgets drawn directly with a test-owned model (no document, file or
    // Diagnostics). Action rows are located through the glow service (DrawRow
    // calls it for EVERY row while RowWithThumb's Selectable is the last item);
    // map rows through the state probe seam.
    struct KeysUi : Ui
    {
        Arcane::Editor::InputActionsEditorModel model{ nlohmann::json::parse(kDoc) };
        Arcane::Editor::InputActionsDocumentState state;
        Arcane::Editor::InputActionsDocumentWidgets widgets;
        Arcane::Editor::InputActionsDocumentWidgets::Services services;
        KeysUi()
        {
            state.probe = &probe;
            services.glow = [this](const Guid& id) {
                const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
                probe[id.ToString()] = ImVec2(lo.x + 40.0f, (lo.y + hi.y) * 0.5f);
                return 0.0f; };
        }
        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_Always);
            ImGui::Begin("Input##keys");
            widgets.Draw(model, state, services);
            ImGui::End();
            ImGui::Render();
        }
        void Click(ImVec2 at, int b = 0)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(at.x, at.y); Frame();
            io.AddMouseButtonEvent(b, true); Frame();
            io.AddMouseButtonEvent(b, false); Frame();
        }
        void Key(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, true); Frame(); ImGui::GetIO().AddKeyEvent(k, false); Frame(); }
    };
    const char* kPlayer = "22222222-2222-4222-8222-222222222222";
    const char* kUi = "55555555-5555-4555-8555-555555555555";
}

TEST_CASE("input document: Delete under a map row's context menu never deletes the SELECTED map (4e9796ba, I2)", "[editor][input]")
{
    KeysUi ui;
    ui.model.SelectMap(G(kPlayer));
    ui.Frame();
    ui.Click(ui.At(kPlayer));
    ui.Click(ui.At(kUi), 1);                                  // right-click UI: its menu opens, selection stays
    REQUIRE(Ui::AnyPopup());
    REQUIRE(ui.model.SelectedMap() == G(kPlayer));
    ui.Key(ImGuiKey_Delete);
    CHECK(ui.model.Draft()["actionMaps"].size() == 2);         // pre-fix: Player deleted
    CHECK(ui.model.SelectedMap() == G(kPlayer));
    // Positive control: close the menu with a click on the row (Escape does not
    // close popups here: no NavEnableKeyboard), select Player, Delete removes it.
    ui.Click(ui.At(kPlayer));
    ui.Click(ui.At(kPlayer));
    REQUIRE_FALSE(Ui::AnyPopup());
    ui.Key(ImGuiKey_Delete);
    CHECK(ui.model.Draft()["actionMaps"].size() == 1);
    CHECK(ui.model.Draft()["actionMaps"][0]["id"] == kUi);
}

TEST_CASE("input document: Delete under an action row's context menu never deletes the SELECTED action", "[editor][input]")
{
    KeysUi ui;
    const char* kJump = "33333333-3333-4333-8333-333333333333";
    const char* kCrouch = "66666666-6666-4666-8666-666666666666";
    ui.model.SelectMap(G(kPlayer)); ui.model.SelectAction(G(kJump));
    ui.Frame();
    ui.Click(ui.At(kJump));
    ui.Click(ui.At(kCrouch), 1);
    REQUIRE(Ui::AnyPopup());
    REQUIRE(ui.model.SelectedAction() == G(kJump));
    ui.Key(ImGuiKey_Delete);
    CHECK(ui.model.Draft()["actionMaps"][0]["actions"].size() == 2);
    ui.Click(ui.At(kJump));
    ui.Click(ui.At(kJump));
    REQUIRE_FALSE(Ui::AnyPopup());
    ui.Key(ImGuiKey_Delete);
    CHECK(ui.model.Draft()["actionMaps"][0]["actions"].size() == 1);
    CHECK(ui.model.Draft()["actionMaps"][0]["actions"][0]["id"] == kCrouch);
}
