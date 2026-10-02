// Device-less ImGui tests for the Input Actions document (the PropertyGridTest
// harness shape: own context, software font atlas, windows pinned, events
// injected between frames). Covers what only ImGui can observe: the page's
// Rebind focus (final review I1), keys idle under a context menu (I2), the
// page's validated Name row (arc-1 debt B), and the pending add-and-listen
// capture (node-page spec s8.3: nothing enters the draft until it ends).
#include <catch2/catch_test_macros.hpp>
#include "Documents/InputActionsDocument.hpp"
#include "Documents/InputActionsDocumentWidgets.hpp"
#include "Documents/InputActionsEditorModel.hpp"
#include "Documents/InputPendingAdd.hpp"
#include "Helpers/TestTypeContext.hpp"
#include "Widgets/PropertyGrid.hpp"
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <imgui.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

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

    // A scheme, a simple binding, a 2D composite whose parts inherit a group,
    // and a grouped binding: every Rebind-column trailing shape and every add.
    const char* kCaptureDoc = R"JSON({
        "version":1,"id":"11111111-1111-4111-8111-111111111111","defaultMap":"22222222-2222-4222-8222-222222222222",
        "controlSchemes":[{"id":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","name":"Keyboard and Mouse","bindingGroup":"KeyboardMouse"}],
        "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"Player","actions":[
            {"id":"33333333-3333-4333-8333-333333333333","name":"Jump","type":"Button","bindings":[{"id":"44444444-4444-4444-8444-444444444444","path":"<Keyboard>/space"}]},
            {"id":"88888888-8888-4888-8888-888888888888","name":"Move","type":"Axis2D","bindings":[
              {"id":"99999999-9999-4999-8999-999999999999","composite":"2DVector","groups":["KeyboardMouse"],"parts":[
                {"id":"a0000001-0000-4000-8000-000000000001","name":"up","path":"<Keyboard>/i"},
                {"id":"a0000002-0000-4000-8000-000000000002","name":"down","path":"<Keyboard>/k"},
                {"id":"a0000003-0000-4000-8000-000000000003","name":"left","path":"<Keyboard>/j"},
                {"id":"a0000004-0000-4000-8000-000000000004","name":"right","path":"<Keyboard>/l"}]}]},
            {"id":"66666666-6666-4666-8666-666666666666","name":"Crouch","type":"Button","bindings":[{"id":"77777777-7777-4777-8777-777777777777","path":"<Keyboard>/c","groups":["KeyboardMouse"]}]}]}]})JSON";
    const char* kMapId = "22222222-2222-4222-8222-222222222222";
    const char* kJumpId = "33333333-3333-4333-8333-333333333333";
    const char* kJumpBinding = "44444444-4444-4444-8444-444444444444";
    const char* kMoveId = "88888888-8888-4888-8888-888888888888";
    const char* kMoveComposite = "99999999-9999-4999-8999-999999999999";
    const char* kCrouchId = "66666666-6666-4666-8666-666666666666";
    const char* kCrouchBinding = "77777777-7777-4777-8777-777777777777";

    struct UndoRig
    {
        Arcane::Runtime runtime{ Arcane::Test::Process() };
        Arcane::CommandStack commands{ [this]() -> Astra::Registry& { return runtime.Registry(); } };
    };

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
        explicit DocUi(const char* json = kDoc, Arcane::CommandStack* commands = nullptr)
        {
            path = fs::temp_directory_path() / ("ui-" + Guid::Generate().ToString() + ".arcinput");
            { std::ofstream out(path); out << json; }
            doc = Arcane::Editor::InputActionsDocument::Open(path, [commands]() -> Arcane::CommandStack* { return commands; });   // T1-B13: Open takes an UndoResolver; a null stack resolves to null = no push
            grid.probe = &probe;
            if (doc) doc->MutableState().probe = &probe;   // rows record their centres (TEST SEAM)
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
        void Click(ImVec2 p) { Move(p); Button(0, true); Button(0, false); }
        void DoubleClick(ImVec2 p) { Move(p); Button(0, true); Button(0, false); Button(0, true); Button(0, false); }
        // The capture's snapshot (SDL's, fed by the app): down for one frame, then up.
        void Press(std::uint32_t scancode)
        {
            Arcane::InputSnapshot s; s.SetScancode(scancode);
            doc->SetPreviewSnapshot(s); Frame();
            doc->SetPreviewSnapshot(Arcane::InputSnapshot{}); Frame();
        }
        // Empty actions-column space: focuses the document (a capture needs it);
        // the deselect to the map is silent.
        void FocusDoc() { Click(ImVec2(500.0f, 585.0f)); }
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

TEST_CASE("input document: a pending binding enters nothing while it listens; W commits ONE binding with its groups, one undo step", "[editor][input]")
{
    UndoRig rig;
    DocUi ui(kCaptureDoc, &rig.commands);
    REQUIRE(ui.doc);
    ui.Frame(); ui.Frame();
    ui.FocusDoc();
    const nlohmann::json before = ui.doc->Model().Draft();
    ui.doc->BeginPending(Arcane::Editor::MakeAddBinding(G(kMapId), G(kJumpId), { "KeyboardMouse" }));
    ui.Frame(); ui.Frame();
    CHECK(ui.doc->InputSwallowed());
    REQUIRE(ui.doc->Pending());
    CHECK(ui.doc->Model().Draft() == before);           // listening: nothing in the draft
    CHECK_FALSE(rig.commands.CanUndo());
    ui.Press(26);                                         // W
    CHECK_FALSE(ui.doc->Pending());
    const nlohmann::json bindings = ui.doc->Model().Draft()["actionMaps"][0]["actions"][0]["bindings"];
    REQUIRE(bindings.size() == 2);
    CHECK(bindings[1]["path"] == "<Keyboard>/scancode/w");
    CHECK(bindings[1]["groups"] == nlohmann::json::array({ "KeyboardMouse" }));
    CHECK(std::string(rig.commands.UndoLabel()) == "Add binding");
    REQUIRE(ui.doc->Model().Undo());
    CHECK(ui.doc->Model().Draft() == before);
    CHECK_FALSE(rig.commands.CanUndo());                 // exactly one step
}

TEST_CASE("input document: Esc on the first part of a pending add adds nothing and pushes nothing", "[editor][input]")
{
    UndoRig rig;
    DocUi ui(kCaptureDoc, &rig.commands);
    ui.Frame(); ui.Frame();
    ui.FocusDoc();
    const nlohmann::json before = ui.doc->Model().Draft();
    ui.doc->BeginPending(Arcane::Editor::MakeAddBinding(G(kMapId), G(kJumpId), {}));
    ui.Frame();
    ui.Key(ImGuiKey_Escape);
    ui.Frame();
    CHECK_FALSE(ui.doc->InputSwallowed());
    CHECK_FALSE(ui.doc->Pending());
    CHECK(ui.doc->Model().Draft() == before);
    CHECK_FALSE(rig.commands.CanUndo());
}

TEST_CASE("input document: 2D Vector, W, then Esc gives ONE composite with ONE part (up); the held W never feeds the next role", "[editor][input]")
{
    UndoRig rig;
    DocUi ui(kCaptureDoc, &rig.commands);
    ui.Frame(); ui.Frame();
    ui.FocusDoc();
    ui.doc->BeginPending(Arcane::Editor::MakeAddComposite(G(kMapId), G(kJumpId), "2DVector", {}));
    ui.Frame();
    ui.Press(26);                                         // W -> up; "down" listens at once
    REQUIRE(ui.doc->Pending());
    CHECK(ui.doc->Pending()->captured == std::vector<std::string>{ "<Keyboard>/scancode/w" });
    CHECK(ui.doc->InputSwallowed());
    ui.Key(ImGuiKey_Escape);                              // a later Esc commits what was heard
    CHECK_FALSE(ui.doc->Pending());
    const nlohmann::json composite = ui.doc->Model().Draft()["actionMaps"][0]["actions"][0]["bindings"][1];
    CHECK(composite["composite"] == "2DVector");
    REQUIRE(composite["parts"].size() == 1);
    CHECK(composite["parts"][0]["name"] == "up");
    CHECK(composite["parts"][0]["path"] == "<Keyboard>/scancode/w");
    CHECK_FALSE(composite.contains("groups"));
    CHECK(std::string(rig.commands.UndoLabel()) == "Add composite binding");
}

TEST_CASE("input document: a click outside the document mid-composite commits the parts heard so far in ONE step", "[editor][input]")
{
    UndoRig rig;
    DocUi ui(kCaptureDoc, &rig.commands);
    ui.Frame(); ui.Frame();
    ui.FocusDoc();
    ui.doc->BeginPending(Arcane::Editor::MakeAddComposite(G(kMapId), G(kJumpId), "1DAxis", {}));
    ui.Frame();
    ui.Press(26);                                         // W -> negative; "positive" listens
    REQUIRE(ui.doc->Pending());
    ui.Click(ImVec2(1200.0f, 850.0f));                    // into the Inspector: TickCapture's clickedAway cancels
    CHECK_FALSE(ui.doc->Pending());
    CHECK_FALSE(ui.doc->InputSwallowed());
    const nlohmann::json composite = ui.doc->Model().Draft()["actionMaps"][0]["actions"][0]["bindings"][1];
    REQUIRE(composite["parts"].size() == 1);
    CHECK(composite["parts"][0]["name"] == "negative");
    CHECK(composite["parts"][0]["path"] == "<Keyboard>/scancode/w");
    CHECK(std::string(rig.commands.UndoLabel()) == "Add composite binding");
    REQUIRE(ui.doc->Model().Undo());
    CHECK_FALSE(rig.commands.CanUndo());                 // exactly one step
}

TEST_CASE("input document: a pending add whose action vanished commits nothing", "[editor][input]")
{
    UndoRig rig;
    DocUi ui(kCaptureDoc, &rig.commands);
    ui.Frame(); ui.Frame();
    ui.FocusDoc();
    ui.doc->BeginPending(Arcane::Editor::MakeAddBinding(G(kMapId), G(kJumpId), {}));
    ui.Frame();
    REQUIRE(ui.doc->Model().RemoveAction(G(kMapId), G(kJumpId)));   // gone while the capture listens
    ui.Press(26);
    ui.Frame();
    CHECK_FALSE(ui.doc->Pending());
    CHECK_FALSE(ui.doc->InputSwallowed());
    CHECK(ui.doc->Model().FindNode(G(kJumpId)) == nullptr);
    CHECK(std::string(rig.commands.UndoLabel()) == "Remove action");   // the refused commit pushed nothing
}
