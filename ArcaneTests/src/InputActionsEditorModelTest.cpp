#include <catch2/catch_test_macros.hpp>

#include "Documents/InputActionsEditorModel.hpp"
#include "Documents/InputActionsDocument.hpp"
#include "Documents/DocumentHost.hpp"

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Base/Runtime.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace
{
    nlohmann::json DocumentJson()
    {
        return nlohmann::json::parse(R"JSON({
            "version":1,"id":"11111111-1111-4111-8111-111111111111",
            "defaultMap":"22222222-2222-4222-8222-222222222222","controlSchemes":[],
            "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"Player","actions":[
              {"id":"33333333-3333-4333-8333-333333333333","name":"Jump","type":"Button","bindings":[
                {"id":"44444444-4444-4444-8444-444444444444","path":"<Keyboard>/space"}]}
            ]}]
        })JSON");
    }
}

TEST_CASE("input editor: stable selection and one-step undo redo", "[editor][input]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::CommandStack commands([&]() -> Astra::Registry& { return runtime.Registry(); });
    Arcane::Editor::InputActionsEditorModel model(DocumentJson(), &commands);
    REQUIRE(model.LastValidPreview());
    const auto selected = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    model.SelectMap(*Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222"));
    model.SelectAction(selected);
    auto after = model.Draft();
    after["actionMaps"][0]["actions"][0]["name"] = "Leap";
    REQUIRE(model.ApplyEdit("Rename action", model.Draft(), after));
    CHECK(model.Dirty());
    CHECK(model.SelectedAction() == selected);
    CHECK(model.LastValidPreview()->actionMaps[0].actions[0].name == "Leap");
    REQUIRE(model.Undo());
    CHECK(model.Draft()["actionMaps"][0]["actions"][0]["name"] == "Jump");
    CHECK(model.SelectedAction() == selected);
    REQUIRE(model.Redo());
    CHECK(model.Draft()["actionMaps"][0]["actions"][0]["name"] == "Leap");
}

TEST_CASE("input editor: invalid draft retains the last valid preview", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    REQUIRE(model.LastValidPreview());
    auto invalid = model.Draft();
    invalid["version"] = 99;
    REQUIRE(model.ApplyEdit("Break schema", model.Draft(), invalid));
    CHECK(model.Dirty());
    CHECK_FALSE(model.Diagnostics().empty());
    CHECK(model.LastValidPreview()->actionMaps[0].actions[0].name == "Jump");
    CHECK_FALSE(model.Save(std::filesystem::temp_directory_path() / "should-not-save.arcinput"));
}

TEST_CASE("input editor: duplicate action mints fresh subtree IDs", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto action = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    REQUIRE(model.DuplicateAction(map, action));
    const auto& actions = model.Draft()["actionMaps"][0]["actions"];
    REQUIRE(actions.size() == 2);
    CHECK(actions[0]["id"] != actions[1]["id"]);
    CHECK(actions[0]["bindings"][0]["id"] != actions[1]["bindings"][0]["id"]);
    REQUIRE(model.LastValidPreview());
}

TEST_CASE("input editor: document saves and reloads a valid asset", "[editor][input]")
{
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() /
        ("arcane_input_document_" + Arcane::Guid::Generate().ToString() + ".arcinput");
    std::ofstream(path) << DocumentJson().dump(2);
    auto document = Arcane::Editor::InputActionsDocument::Open(path);
    REQUIRE(document);
    auto after = document->Model().Draft();
    after["actionMaps"][0]["actions"][0]["name"] = "Leap";
    REQUIRE(document->Model().ApplyEdit("Rename", document->Model().Draft(), after));
    REQUIRE(document->Save());
    CHECK_FALSE(document->Dirty());
    auto reopened = Arcane::Editor::InputActionsDocument::Open(path);
    REQUIRE(reopened);
    CHECK(reopened->Model().Draft()["actionMaps"][0]["actions"][0]["name"] == "Leap");
    std::error_code error;
    fs::remove(path, error);
}

TEST_CASE("input editor: malformed source opens for repair without overwriting it", "[editor][input]")
{
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() /
        ("arcane_input_repair_" + Arcane::Guid::Generate().ToString() + ".arcinput");
    std::ofstream(path) << "{broken";
    auto document = Arcane::Editor::InputActionsDocument::Open(path);
    REQUIRE(document);
    CHECK_FALSE(document->Model().LastValidPreview());
    CHECK_FALSE(document->Model().Diagnostics().empty());
    CHECK_FALSE(document->Save());
    {
        std::ifstream in(path);
        CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == "{broken");
    }
    REQUIRE(document->Model().ApplyEdit("Repair JSON", document->Model().Draft(), DocumentJson()));
    REQUIRE(document->Save());
    CHECK(document->Model().LastValidPreview().has_value());
    std::error_code error;
    fs::remove(path, error);
}

TEST_CASE("input editor: opening the same asset focuses its document", "[editor][input]")
{
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() /
        ("arcane_input_focus_" + Arcane::Guid::Generate().ToString() + ".arcinput");
    std::ofstream(path) << DocumentJson().dump(2);
    Arcane::Editor::DocumentHost host;
    host.RegisterFactory(".arcinput",
        [](const fs::path& source) { return Arcane::Editor::InputActionsDocument::Open(source); },
        [](const fs::path& source) { return Arcane::Editor::InputActionsDocument::PeekGuid(source); });
    auto* first = host.OpenPath(path);
    REQUIRE(first);
    CHECK(host.OpenPath(path) == first);
    CHECK(host.Count() == 1);
    host.CloseAll();
    std::error_code error;
    fs::remove(path, error);
}

TEST_CASE("input editor: map lifecycle maintains default and undo", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(Arcane::InputActionAsset::CreateDefault().ToJson());
    REQUIRE(model.AddMap("Player"));
    const auto player = model.SelectedMap();
    CHECK(model.Draft()["defaultMap"] == player.ToString());
    REQUIRE(model.AddMap("Menus"));
    const auto menus = model.SelectedMap();
    REQUIRE(model.SetDefaultMap(menus));
    REQUIRE(model.MoveRow(menus, -1));
    CHECK(model.Draft()["actionMaps"][0]["id"] == menus.ToString());
    REQUIRE(model.RemoveMap(menus));
    CHECK(model.Draft()["defaultMap"] == player.ToString());
    REQUIRE(model.RemoveMap(player));
    CHECK_FALSE(model.Draft().contains("defaultMap"));
    REQUIRE(model.LastValidPreview());
}

TEST_CASE("input editor: action binding composite and scheme edits", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto action = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    REQUIRE(model.AddScheme("Gamepad", "Gamepad"));
    REQUIRE(model.SetField(action, "type", "Axis1D"));
    REQUIRE(model.AddComposite(map, action, "1DAxis"));
    const auto binding = model.SelectedBinding();
    REQUIRE(model.LastValidPreview());
    CHECK(model.Draft()["actionMaps"][0]["actions"][0]["bindings"].size() == 2);
    REQUIRE(model.SetField(binding, "groups", nlohmann::json::array({"Gamepad"})));
    REQUIRE(model.MoveRow(binding, -1));
    REQUIRE(model.RemoveBinding(map, action, binding));
    REQUIRE(model.LastValidPreview());
    CHECK(model.Draft()["actionMaps"][0]["actions"][0]["bindings"].size() == 1);
}

TEST_CASE("input editor: same path warnings remain nonblocking", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto action = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    REQUIRE(model.AddBinding(map, action, "<Keyboard>/space"));
    CHECK_FALSE(model.Warnings().empty());
    REQUIRE(model.LastValidPreview());
}

TEST_CASE("input editor: composite parts can be added removed and reordered", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto action = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    REQUIRE(model.AddComposite(map, action, "1DAxis"));
    const auto binding = model.SelectedBinding();
    REQUIRE(model.AddPart(binding, "positive", "<Keyboard>/d"));
    const auto part = model.SelectedPart();
    REQUIRE(model.MoveRow(part, -1));
    REQUIRE(model.DuplicateRow(binding));
    REQUIRE(model.RemovePart(binding, part));
    CHECK(model.LastValidPreview().has_value());
    CHECK(model.Draft()["actionMaps"][0]["actions"][0]["bindings"].size() == 3);
}

TEST_CASE("input editor: renaming a scheme preserves binding membership", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto action = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    REQUIRE(model.AddScheme("Keyboard", "Keyboard"));
    const auto scheme = *Arcane::Guid::FromString(
        model.Draft()["controlSchemes"][0]["id"].get<std::string>());
    REQUIRE(model.AddBinding(map, action, "<Keyboard>/a"));
    const auto binding = model.SelectedBinding();
    REQUIRE(model.SetField(binding, "groups", nlohmann::json::array({"Keyboard"})));
    REQUIRE(model.EditScheme(scheme, "Keyboard and Mouse", "KeyboardMouse"));
    CHECK(model.Draft()["controlSchemes"][0]["name"] == "Keyboard and Mouse");
    CHECK(model.Draft()["actionMaps"][0]["actions"][0]["bindings"][1]["groups"][0] ==
          "KeyboardMouse");
    CHECK(model.LastValidPreview().has_value());
}

TEST_CASE("input editor: a document selects its first map and action on open", "[editor][input]")
{
    // A freshly opened document shows something, as Unity's editor does: the
    // first map and that map's first action are selected, so the hierarchy
    // and the properties are on screen rather than an empty "Select an
    // action map." pane (hygiene pass 2026-09-28, the --open-asset capture).
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() /
        ("arcane_input_document_" + Arcane::Guid::Generate().ToString() + ".arcinput");
    std::ofstream(path) << DocumentJson().dump(2);
    auto document = Arcane::Editor::InputActionsDocument::Open(path);
    REQUIRE(document);
    const auto& draft = document->Model().Draft();
    const auto firstMap    = Arcane::Guid::FromString(draft["actionMaps"][0]["id"].get<std::string>());
    const auto firstAction = Arcane::Guid::FromString(draft["actionMaps"][0]["actions"][0]["id"].get<std::string>());
    REQUIRE(firstMap);
    REQUIRE(firstAction);
    CHECK(document->Model().SelectedMap() == *firstMap);
    CHECK(document->Model().SelectedAction() == *firstAction);
    CHECK_FALSE(document->Model().SelectedBinding().IsValid());
    fs::remove(path);
}

TEST_CASE("input editor: selection epoch, key and restore", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto action = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    const auto binding = *Arcane::Guid::FromString("44444444-4444-4444-8444-444444444444");
    CHECK(model.SelectionKey().empty());
    const auto e0 = model.SelectionEpoch();
    model.SelectMap(map);
    CHECK(model.SelectionEpoch() == e0 + 1);
    model.SelectMap(map);                                  // re-select: a gesture, bumps
    CHECK(model.SelectionEpoch() == e0 + 2);
    model.SelectAction(action);
    model.SelectBinding(binding);
    CHECK(model.SelectionEpoch() == e0 + 4);
    const std::string key = model.SelectionKey();
    CHECK(key == map.ToString() + "/" + action.ToString() + "/" + binding.ToString() + "/");
    model.SelectMap({});
    CHECK(model.SelectionKey().empty());
    REQUIRE(model.RestoreSelection(key));
    CHECK(model.SelectedBinding() == binding);
    CHECK(model.SelectedAction() == action);
    REQUIRE(model.RemoveBinding(map, action, binding));
    CHECK_FALSE(model.Resolves(key));
    const auto eR = model.SelectionEpoch();
    CHECK_FALSE(model.RestoreSelection(key));              // the binding is gone: unresolvable
    CHECK(model.SelectionEpoch() == eR);                   // Resolves/RestoreSelection failure: pure
    CHECK(model.FindNode(action) != nullptr);
    CHECK(model.FindNode(binding) == nullptr);
    CHECK_FALSE(model.RestoreSelection("not-a-key"));
}

TEST_CASE("input editor: undo of a structural edit restores a live selection, silently", "[editor][input]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::CommandStack commands([&]() -> Astra::Registry& { return runtime.Registry(); });
    Arcane::Editor::InputActionsEditorModel model(DocumentJson(), &commands);
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto action = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    model.SelectMap(map); model.SelectAction(action);
    REQUIRE(model.AddBinding(map, action, "<Keyboard>/w"));      // selects the new binding
    const auto added = model.SelectedBinding();
    REQUIRE(added.IsValid());
    const auto e0 = model.SelectionEpoch();
    commands.Undo();                                              // the STACK, as EditorAppFrame.cpp:885 drives it
    CHECK_FALSE(model.SelectedBinding().IsValid());
    CHECK(model.SelectedAction() == action);
    CHECK(model.SelectedMap() == map);
    CHECK(model.SelectionEpoch() == e0);                          // silent: a mechanical change is not a gesture
    commands.Redo();
    CHECK(model.SelectedBinding() == added);
    REQUIRE(model.RemoveBinding(map, action, added));
    commands.Undo();
    CHECK(model.SelectedBinding() == added);                      // undo of a delete reselects what was live when it was deleted
}

TEST_CASE("input editor: SelectByPath resolves names and binding indices", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    REQUIRE(model.SelectByPath("Player"));
    CHECK(model.SelectedMap().ToString() == "22222222-2222-4222-8222-222222222222");
    CHECK_FALSE(model.SelectedAction().IsValid());
    REQUIRE(model.SelectByPath("Player/Jump"));
    CHECK(model.SelectedAction().ToString() == "33333333-3333-4333-8333-333333333333");
    REQUIRE(model.SelectByPath("Player/Jump/0"));
    CHECK(model.SelectedBinding().ToString() == "44444444-4444-4444-8444-444444444444");
    CHECK_FALSE(model.SelectByPath("Player/Crouch"));
    CHECK_FALSE(model.SelectByPath("Enemy"));
    CHECK_FALSE(model.SelectByPath("Player/Jump/7"));
    CHECK_FALSE(model.SelectByPath(""));
}

TEST_CASE("input editor: MoveRowTo reorders within the parent and is one undo step", "[editor][input]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::CommandStack commands([&]() -> Astra::Registry& { return runtime.Registry(); });
    Arcane::Editor::InputActionsEditorModel model(DocumentJson(), &commands);
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto action = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    REQUIRE(model.AddBinding(map, action, "<Keyboard>/w"));
    REQUIRE(model.AddBinding(map, action, "<Gamepad>/buttonSouth"));
    const auto& bindings = model.Draft()["actionMaps"][0]["actions"][0]["bindings"];
    const auto south = *Arcane::Guid::FromString(bindings[2]["id"].get<std::string>());
    REQUIRE(model.MoveRowTo(south, 0));
    CHECK(model.Draft()["actionMaps"][0]["actions"][0]["bindings"][0]["path"] == "<Gamepad>/buttonSouth");
    CHECK_FALSE(model.MoveRowTo(south, 0));                // already there: not an edit
    REQUIRE(model.MoveRowTo(south, 99));                   // clamps to the last slot
    CHECK(model.Draft()["actionMaps"][0]["actions"][0]["bindings"][2]["path"] == "<Gamepad>/buttonSouth");
    REQUIRE(model.Undo());
    CHECK(model.Draft()["actionMaps"][0]["actions"][0]["bindings"][0]["path"] == "<Gamepad>/buttonSouth");
    CHECK_FALSE(model.MoveRowTo(Arcane::Guid::Generate(), 0));
}

TEST_CASE("input editor: names are validated and kept unique among siblings", "[editor][input]")
{
    using M = Arcane::Editor::InputActionsEditorModel;
    M model(DocumentJson());
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto jump = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    CHECK(M::ValidateName(model.Draft(), jump, "") == "Names cannot be blank");
    CHECK(M::ValidateName(model.Draft(), jump, "   ").has_value());
    CHECK_FALSE(M::ValidateName(model.Draft(), jump, "Jump").has_value());       // unchanged
    CHECK_FALSE(M::ValidateName(model.Draft(), jump, " Jump ").has_value());     // trimmed, unchanged
    REQUIRE(model.AddAction(map));  const auto a1 = model.SelectedAction();     // AddAction selects the new row (:274-286)
    REQUIRE(model.AddAction(map));  const auto a2 = model.SelectedAction();
    CHECK(model.FindNode(a1)->at("name") == "Action");
    CHECK(model.FindNode(a2)->at("name") == "Action 2");
    CHECK(M::ValidateName(model.Draft(), a2, "Jump") == "Another action in this map is already named 'Jump'");
    CHECK(model.SiblingNameTaken(a2, "Jump"));
    CHECK_FALSE(model.SiblingNameTaken(jump, "Jump"));                           // self
    const auto before = model.Draft();
    CHECK_FALSE(model.SetField(a2, "name", "Jump"));                             // refused: no edit
    CHECK(model.Draft() == before);
    REQUIRE(model.SetField(a2, "name", "  Crouch  "));
    CHECK(model.FindNode(a2)->at("name") == "Crouch");                           // trimmed
    // DuplicateAction inserts the copy right after the original (:230-233) and
    // does NOT select it: read the copy's name from the draft at index 1.
    REQUIRE(model.DuplicateAction(map, jump)); CHECK(model.Draft()["actionMaps"][0]["actions"][1]["name"] == "Jump Copy");
    REQUIRE(model.DuplicateAction(map, jump)); CHECK(model.Draft()["actionMaps"][0]["actions"][1]["name"] == "Jump Copy 2");
    REQUIRE(model.AddMap()); CHECK(model.FindNode(model.SelectedMap())->at("name") == "Action Map");   // AddMap selects the new map (:240-251)
    REQUIRE(model.AddMap()); CHECK(model.FindNode(model.SelectedMap())->at("name") == "Action Map 2");
    CHECK(M::ValidateName(model.Draft(), model.SelectedMap(), "Player") == "A map named 'Player' already exists");
    // A legacy file with two "Jump"s: Problems line + ambiguous SelectByPath refused.
    auto dup = DocumentJson();
    dup["actionMaps"][0]["actions"].push_back(nlohmann::json::parse(R"({"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","name":"Jump","type":"Button","bindings":[]})"));
    M legacy(std::move(dup));
    const auto legacyWarnings = legacy.Warnings();   // one vector: begin/end of two temporaries would be UB
    CHECK(std::any_of(legacyWarnings.begin(), legacyWarnings.end(), [](const std::string& w) { return w.starts_with("Invalid name in Player/Jump"); }));
    CHECK_FALSE(legacy.SelectByPath("Player/Jump"));
}

TEST_CASE("input editor: Conflicts and Warnings -- same scheme, ungrouped-vs-grouped, spelling-independent, unknown paths", "[editor][input]")
{
    auto json = DocumentJson();
    json["controlSchemes"] = nlohmann::json::array({
        { {"id", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"}, {"name", "KeyboardMouse"}, {"bindingGroup", "KeyboardMouse"} },
        { {"id", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"}, {"name", "Gamepad"}, {"bindingGroup", "Gamepad"} } });
    auto& actions = json["actionMaps"][0]["actions"];
    actions[0]["bindings"][0]["groups"] = { "KeyboardMouse" };                        // Jump: space (KBM)
    // Parsed from text, not brace-initialised: nlohmann's initializer-list
    // rule turns a one-element list of pairs into an OBJECT, so a single-
    // binding "bindings" array written with braces would not be an array.
    actions.push_back(nlohmann::json::parse(R"JSON({"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","name":"Crouch","type":"Button",
        "bindings":[{"id":"dddddddd-dddd-4ddd-8ddd-dddddddddddd","path":"<Keyboard>/scancode/space"}]})JSON"));   // ungrouped space, in the CAPTURE's spelling
    actions.push_back(nlohmann::json::parse(R"JSON({"id":"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee","name":"Fire","type":"Button",
        "bindings":[{"id":"ffffffff-ffff-4fff-8fff-ffffffffffff","path":"<Keyboard>/space","groups":["Gamepad"]},
                    {"id":"12121212-1212-4121-8121-121212121212","path":"<Keyboard>/spaec"}]})JSON"));
    actions.push_back(nlohmann::json::parse(R"JSON({"id":"13131313-1313-4131-8131-131313131313","name":"Aim","type":"Button",
        "bindings":[{"id":"14141414-1414-4141-8141-141414141414","path":"<Mouse>/button/1"},
                    {"id":"15151515-1515-4151-8151-151515151515","path":"<Keyboard>/lshift+<Keyboard>/a"}]})JSON"));   // ungrouped
    actions.push_back(nlohmann::json::parse(R"JSON({"id":"16161616-1616-4161-8161-161616161616","name":"Block","type":"Button",
        "bindings":[{"id":"17171717-1717-4171-8171-171717171717","path":"<Mouse>/leftButton"},
                    {"id":"18181818-1818-4181-8181-181818181818","path":"<Keyboard>/a+<Keyboard>/lshift"}]})JSON"));   // ungrouped
    Arcane::Editor::InputActionsEditorModel model(std::move(json));
    REQUIRE(model.LastValidPreview());
    const auto conflicts = model.Conflicts();
    auto conflictsOf = [&](const char* id) {
        std::vector<std::string> others;
        for (const auto& c : conflicts) if (c.binding.ToString() == id) others.push_back(c.otherActionName);
        return others; };
    // Jump/space (KBM) vs Crouch/scancode/space (ungrouped): the same control, a conflict. Jump vs Fire (Gamepad group): NOT a conflict.
    CHECK(conflictsOf("44444444-4444-4444-8444-444444444444") == std::vector<std::string>{ "Crouch" });
    // Crouch (ungrouped) conflicts with BOTH grouped ones.
    const auto crouch = conflictsOf("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    CHECK(crouch.size() == 2);
    CHECK(conflictsOf("ffffffff-ffff-4fff-8fff-ffffffffffff") == std::vector<std::string>{ "Crouch" });
    CHECK(conflictsOf("14141414-1414-4141-8141-141414141414") == std::vector<std::string>{ "Block" });   // <Mouse>/button/1 IS <Mouse>/leftButton
    CHECK(conflictsOf("15151515-1515-4151-8151-151515151515") == std::vector<std::string>{ "Block" });   // chord parts in either order
    CHECK(conflictsOf("12121212-1212-4121-8121-121212121212").empty());                                  // 'spaec' compiles to nothing: conflicts with nothing
    const auto warnings = model.Warnings();
    CHECK(std::count_if(warnings.begin(), warnings.end(), [](const std::string& w) { return w.starts_with("Unknown control path"); }) == 1);
    CHECK(std::count_if(warnings.begin(), warnings.end(), [](const std::string& w) { return w.starts_with("Conflicting"); }) == 4);   // one per PAIR: Jump/Crouch, Fire/Crouch, Aim/Block mouse, Aim/Block chord
}

TEST_CASE("input document: the capture snapshot ignores ImGui's mouse claim and keeps the keyboard's ActiveId claim", "[editor][input]")
{
    Arcane::InputSnapshot raw; raw.mouseButtons = 0x2; raw.wantCaptureMouse = true; raw.wantCaptureKeyboard = false;
    const auto s = Arcane::Editor::InputActionsDocument::SnapshotForCapture(raw, false);
    CHECK_FALSE(s.wantCaptureMouse); CHECK_FALSE(s.wantCaptureKeyboard); CHECK(s.mouseButtons == 0x2);
    CHECK(Arcane::Editor::InputActionsDocument::SnapshotForCapture(raw, true).wantCaptureKeyboard);
}

TEST_CASE("input document: is an Inspector source with keyed pages and a breadcrumb", "[editor][input][inspector]")
{
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "inspector-source-test.arcinput";
    { std::ofstream out(path); out << DocumentJson().dump(2); }
    auto doc = Arcane::Editor::InputActionsDocument::Open(path);
    REQUIRE(doc);
    CHECK(doc->SourceName() == "inspector-source-test.arcinput");
    // Open selects the first map + action (ca171951): the page is the action page.
    const auto e0 = doc->SelectionEpoch();
    REQUIRE(doc->Page() != nullptr);
    auto crumbs = doc->Page()->Breadcrumb();
    REQUIRE(crumbs.size() == 3);
    CHECK(crumbs[0].label == "inspector-source-test.arcinput");
    CHECK(crumbs[1].label == "Player");
    CHECK(crumbs[2].label == "Jump");
    REQUIRE(doc->SelectByPath("Player/Jump/0"));
    CHECK(doc->SelectionEpoch() > e0);
    crumbs = doc->Page()->Breadcrumb();
    REQUIRE(crumbs.size() == 4);
    CHECK(crumbs[2].label == "Jump");
    CHECK(crumbs[3].label == "Space");
    const std::string key = doc->SelectionKey();
    const std::string mapId = "22222222-2222-4222-8222-222222222222", actionId = "33333333-3333-4333-8333-333333333333";
    REQUIRE(crumbs[0].key); CHECK(crumbs[0].key->empty());                       // the asset root re-pins to the asset page
    REQUIRE(crumbs[1].key); CHECK(*crumbs[1].key == mapId + "///");
    REQUIRE(crumbs[2].key); CHECK(*crumbs[2].key == mapId + "/" + actionId + "//");
    REQUIRE(crumbs[3].key); CHECK(*crumbs[3].key == key);
    CHECK(doc->PageFor(*crumbs[1].key) != nullptr);
    CHECK(doc->Resolves(key)); CHECK_FALSE(doc->Resolves("bogus"));
    REQUIRE(crumbs[1].select);
    crumbs[1].select();                                    // the map crumb selects the map
    CHECK(doc->Page()->Breadcrumb().size() == 2);
    REQUIRE(doc->PageFor(key) != nullptr);                 // a pinned page for the binding
    CHECK(doc->PageFor(key)->Breadcrumb().size() == 4);
    CHECK(doc->PageFor("bogus") == nullptr);
    REQUIRE(doc->RestoreSelection(key));
    CHECK(doc->SelectionKey() == key);
    doc->Model().SelectMap({});                            // nothing selected: the ASSET page, never null
    REQUIRE(doc->Page() != nullptr);
    CHECK(doc->Page()->Breadcrumb().size() == 1);
    fs::remove(path);
}

TEST_CASE("input document: Save invokes onSaved exactly once per successful save; a refused save does not", "[editor][input]")
{
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "on-saved.arcinput";
    { std::ofstream out(path); out << DocumentJson().dump(2); }
    auto doc = Arcane::Editor::InputActionsDocument::Open(path);
    REQUIRE(doc);
    int calls = 0;
    Arcane::Guid seen;
    doc->SetOnSaved([&](const Arcane::Guid& g, const Arcane::InputActionAsset&) { ++calls; seen = g; });
    REQUIRE(doc->Save());
    CHECK(calls == 1);
    CHECK(seen == doc->AssetGuid());
    auto invalid = doc->Model().Draft(); invalid["version"] = 99;
    REQUIRE(doc->Model().ApplyEdit("break", doc->Model().Draft(), invalid));
    CHECK_FALSE(doc->Save());                              // refused
    CHECK(calls == 1);                                     // a refused save never fires the callback
    fs::remove(path);
}
