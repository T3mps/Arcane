#include <catch2/catch_test_macros.hpp>

#include "Documents/InputActionsEditorModel.hpp"
#include "Documents/InputActionsDocument.hpp"
#include "Documents/InputSelectionKey.hpp"
#include "Documents/DocumentHost.hpp"
#include "Panels/DiagnosticStore.hpp"

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
    // Final fix R: a re-open re-asserts a NON-EMPTY selection (the asset
    // page's "" key is never a selection event, so nothing to re-assert).
    auto* input = static_cast<Arcane::Editor::InputActionsDocument*>(first);
    input->Model().DeselectToAsset();                  // the asset page: key ""
    REQUIRE(first->SelectionKey().empty());
    const auto epochAsset = first->SelectionEpoch();
    CHECK(host.OpenPath(path) == first);
    CHECK(first->SelectionEpoch() == epochAsset);      // key "" -- no bump
    REQUIRE(input->Model().SelectByPath("Player/Jump"));
    REQUIRE_FALSE(first->SelectionKey().empty());
    const auto epochSelected = first->SelectionEpoch();
    CHECK(host.OpenPath(path) == first);
    CHECK(first->SelectionEpoch() == epochSelected + 1);
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
    const auto w = model.Warnings();
    CHECK(std::find(w.begin(), w.end(), std::string("Conflicting '<Keyboard>/space': Player/Jump binds it twice in every scheme")) != w.end());
    REQUIRE(model.LastValidPreview());
}

TEST_CASE("input editor: a conflict line names the scheme's display name and every overlapping scheme; conflicts stay per map", "[editor][input]")
{
    auto json = nlohmann::json::parse(R"JSON({
        "version":1,"id":"11111111-1111-4111-8111-111111111111","defaultMap":"22222222-2222-4222-8222-222222222222",
        "controlSchemes":[{"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","name":"Keyboard and Mouse","bindingGroup":"KeyboardMouse"},
                          {"id":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","name":"Gamepad","bindingGroup":"Gamepad"}],
        "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"Player","actions":[
            {"id":"33333333-3333-4333-8333-333333333333","name":"Jump","type":"Button","bindings":[
              {"id":"44444444-4444-4444-8444-444444444444","path":"<Keyboard>/space","groups":["KeyboardMouse","Gamepad"]}]},
            {"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","name":"Crouch","type":"Button","bindings":[
              {"id":"dddddddd-dddd-4ddd-8ddd-dddddddddddd","path":"<Keyboard>/space"}]}]},
          {"id":"99999999-9999-4999-8999-999999999999","name":"Menus","actions":[
            {"id":"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee","name":"Confirm","type":"Button","bindings":[
              {"id":"ffffffff-ffff-4fff-8fff-ffffffffffff","path":"<Keyboard>/space"}]}]}]})JSON");
    Arcane::Editor::InputActionsEditorModel model(std::move(json));
    REQUIRE(model.LastValidPreview());
    const auto w = model.Warnings();
    CHECK(std::count_if(w.begin(), w.end(), [](const std::string& s) { return s.starts_with("Conflicting"); }) == 1);
    CHECK(std::find(w.begin(), w.end(), std::string("Conflicting '<Keyboard>/space': Player/Jump and Player/Crouch share it in schemes Keyboard and Mouse, Gamepad")) != w.end());
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
    for (const std::string expected : {
            std::string("Conflicting '<Keyboard>/space': Player/Jump and Player/Crouch share it in scheme KeyboardMouse"),
            std::string("Conflicting '<Keyboard>/scancode/space': Player/Crouch and Player/Fire share it in scheme Gamepad"),
            std::string("Conflicting '<Mouse>/button/1': Player/Aim and Player/Block share it in every scheme"),
            std::string("Conflicting '<Keyboard>/lshift+<Keyboard>/a': Player/Aim and Player/Block share it in every scheme") })
    {
        INFO(expected);
        CHECK(std::find(warnings.begin(), warnings.end(), expected) != warnings.end());
    }
    const auto jumpConflict = std::find_if(conflicts.begin(), conflicts.end(), [](const auto& c) { return c.binding.ToString() == "44444444-4444-4444-8444-444444444444"; });
    REQUIRE(jumpConflict != conflicts.end());
    CHECK(jumpConflict->actionName == "Jump"); CHECK(jumpConflict->otherActionName == "Crouch");
    CHECK(jumpConflict->mapName == "Player"); CHECK(jumpConflict->scheme == "KeyboardMouse");
}

TEST_CASE("input document: the capture snapshot ignores ImGui's mouse claim and keeps the keyboard's ActiveId claim", "[editor][input]")
{
    Arcane::InputSnapshot raw; raw.mouseButtons = 0x2; raw.wantCaptureMouse = true; raw.wantCaptureKeyboard = false;
    const auto s = Arcane::Editor::InputActionsDocument::SnapshotForCapture(raw, false, false);
    CHECK_FALSE(s.wantCaptureMouse); CHECK_FALSE(s.wantCaptureKeyboard); CHECK(s.mouseButtons == 0x2);
    CHECK(Arcane::Editor::InputActionsDocument::SnapshotForCapture(raw, true, false).wantCaptureKeyboard);
    CHECK(Arcane::Editor::InputActionsDocument::SnapshotForCapture(raw, false, true).wantCaptureMouse);
}

TEST_CASE("input document: a press on the window's chrome is claimed -- it neither binds nor is heard later; a content press still binds", "[editor][input]")
{
    using Doc = Arcane::Editor::InputActionsDocument;
    CHECK(Doc::PressOnChrome({ 50, 10 }, { 0, 20 }, { 400, 300 }, 4.0f));    // title bar
    CHECK_FALSE(Doc::PressOnChrome({ 50, 100 }, { 0, 20 }, { 400, 300 }, 4.0f));
    CHECK(Doc::PressOnChrome({ 398, 100 }, { 0, 20 }, { 400, 300 }, 4.0f));  // inner border band
    Arcane::InputRebindOperation op;
    op.Begin(*Arcane::Guid::FromString("44444444-4444-4444-8444-444444444444"), std::nullopt, 10.0f, Arcane::InputSnapshot{});
    Arcane::InputSnapshot down; down.mouseButtons = 0x1;
    op.Observe(Doc::SnapshotForCapture(down, false, true), 1.0f / 60.0f);         // the chrome press
    CHECK(op.Result().state == Arcane::InputRebindState::Waiting);
    op.Observe(Doc::SnapshotForCapture(down, false, false), 1.0f / 60.0f);        // still held, now over content
    CHECK(op.Result().state == Arcane::InputRebindState::Waiting);
    op.Observe(Doc::SnapshotForCapture(Arcane::InputSnapshot{}, false, false), 1.0f / 60.0f);
    CHECK(op.Result().state == Arcane::InputRebindState::Waiting);
    op.Observe(Doc::SnapshotForCapture(down, false, false), 1.0f / 60.0f);        // a fresh press in the content
    CHECK(op.Result().state == Arcane::InputRebindState::Completed);
    CHECK(op.Result().replacementPath == "<Mouse>/leftButton");
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

// Final review C1: a hand-edited .arcinput whose map "name" or "id" is not a
// string opens behind the repair banner; every model read the document's draw
// makes (Warnings, every frame) and every public call must degrade, never
// throw nlohmann's type_error.302.
TEST_CASE("input editor: a non-string map name or id never throws from the model", "[editor][input]")
{
    const std::string mapId = "22222222-2222-4222-8222-222222222222";
    const auto map = *Arcane::Guid::FromString(mapId);
    SECTION("numeric name, valid id")
    {
        Arcane::Editor::InputActionsEditorModel model(nlohmann::json::parse(R"JSON({"actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":5}]})JSON"));
        std::vector<std::string> warnings;
        CHECK_NOTHROW(warnings = model.Warnings());
        REQUIRE(warnings.size() == 1);                        // the non-string name reads as blank: surfaced, not thrown
        CHECK(warnings[0].rfind("Invalid name", 0) == 0);
        std::optional<std::string> why = std::string("unset");
        CHECK_NOTHROW(why = Arcane::Editor::InputActionsEditorModel::ValidateName(model.Draft(), map, "X"));
        CHECK_FALSE(why.has_value());
        bool selected = true;
        CHECK_NOTHROW(selected = model.SelectByPath("Map"));
        CHECK_FALSE(selected);
        CHECK(model.SelectionKey().empty());
        bool removed = false;
        CHECK_NOTHROW(removed = model.RemoveMap(map));   // the id is valid: removal works
        CHECK(removed);
        CHECK(model.Draft()["actionMaps"].empty());
    }
    SECTION("numeric id, string name")
    {
        Arcane::Editor::InputActionsEditorModel model(nlohmann::json::parse(R"JSON({"actionMaps":[{"id":7,"name":"Map"}]})JSON"));
        std::vector<std::string> warnings;
        CHECK_NOTHROW(warnings = model.Warnings());
        CHECK(warnings.empty());
        std::optional<std::string> why = std::string("unset");
        CHECK_NOTHROW(why = Arcane::Editor::InputActionsEditorModel::ValidateName(model.Draft(), map, "X"));
        CHECK_FALSE(why.has_value());
        bool selected = true;
        CHECK_NOTHROW(selected = model.SelectByPath("Map"));   // the name matches, the id does not resolve
        CHECK_FALSE(selected);
        CHECK(model.SelectionKey().empty());
        bool removed = true;
        CHECK_NOTHROW(removed = model.RemoveMap(map));         // no map carries that id
        CHECK_FALSE(removed);
        CHECK(model.Draft()["actionMaps"].size() == 1);
    }
    SECTION("the survivor of a removal carries a malformed id")
    {
        // RemoveMap used to dereference Guid::FromString of the survivor's id.
        Arcane::Editor::InputActionsEditorModel model(nlohmann::json::parse(R"JSON({"defaultMap":"22222222-2222-4222-8222-222222222222","actionMaps":[
            {"id":"22222222-2222-4222-8222-222222222222","name":"A"},{"id":"x","name":"B"}]})JSON"));
        bool removed = false;
        CHECK_NOTHROW(removed = model.RemoveMap(map));
        CHECK(removed);
        CHECK(model.Draft()["actionMaps"].size() == 1);
        CHECK_FALSE(model.Draft().contains("defaultMap"));   // a malformed survivor never becomes the default
        CHECK_FALSE(model.SelectedMap().IsValid());
    }
}

// Ruling P18: a container-fallback deselect INSIDE the document is silent --
// never an event that moves the Inspector away from another source -- while
// the live key (which Page() re-reads each frame) drops to the container.
TEST_CASE("input editor: the container-fallback deselect is silent", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    model.SelectMap(map);
    model.SelectAction(*Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333"));
    const auto epoch = model.SelectionEpoch();
    model.DeselectToMap(map);
    CHECK(model.SelectionEpoch() == epoch);
    CHECK(model.SelectionKey() == "22222222-2222-4222-8222-222222222222///");
    CHECK_FALSE(model.SelectedAction().IsValid());
    model.DeselectToAsset();
    CHECK(model.SelectionEpoch() == epoch);
    CHECK(model.SelectionKey().empty());
    model.SelectMap(map);   // an ordinary Select* still bumps
    CHECK(model.SelectionEpoch() == epoch + 1);
}

TEST_CASE("input editor: Add part from a deselected column selects top-down, so its key resolves", "[editor][input][inspector]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    const auto map = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
    const auto jump = *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    REQUIRE(model.AddComposite(map, jump, "1DAxis"));
    const Arcane::Guid composite = model.SelectedBinding();
    REQUIRE(composite.IsValid());
    model.DeselectToMap(map);                                   // empty space under the rows
    REQUIRE(model.AddPart(composite, "positive", "<Keyboard>/d")); // right-click the header > Add part
    CHECK(model.SelectedMap() == map);
    CHECK(model.SelectedAction() == jump);
    CHECK(model.SelectedBinding() == composite);
    CHECK(model.SelectedPart().IsValid());
    CHECK(model.Resolves(model.SelectionKey()));
}

TEST_CASE("input editor: ParseSelectionKey is the one 4-segment grammar", "[editor][input]")
{
    using Arcane::Editor::ParseSelectionKey;
    using Arcane::Editor::SplitKey;
    const std::string M = "22222222-2222-4222-8222-222222222222", A = "33333333-3333-4333-8333-333333333333",
                      B = "44444444-4444-4444-8444-444444444444", P = "55555555-5555-4555-8555-555555555555";
    CHECK(ParseSelectionKey(M + "///"));
    CHECK(ParseSelectionKey(M + "/" + A + "//"));
    CHECK(ParseSelectionKey(M + "/" + A + "/" + B + "/"));
    REQUIRE(ParseSelectionKey(M + "/" + A + "/" + B + "/" + P));
    CHECK((*ParseSelectionKey(M + "/" + A + "/" + B + "/" + P))[3].ToString() == P);
    for (const std::string bad : { std::string{}, std::string("///"), "/" + A + "//", M, M + "/", M + "//", M + "////",
                                   M + "/" + A + "/" + B + "/" + P + "/", M + "/" + A + "/" + B + "/" + P + "/junk",
                                   M + "//" + B + "/", M + "/bogus//", std::string("00000000-0000-0000-0000-000000000000///"),
                                   std::string("not-a-key") })
    {
        INFO(bad);
        CHECK_FALSE(ParseSelectionKey(bad));
    }
    CHECK(SplitKey("") == std::vector<std::string_view>{ "" });
    CHECK(SplitKey("a/") == std::vector<std::string_view>{ "a", "" });
    CHECK(SplitKey("a//b").size() == 3);
    CHECK(Arcane::Editor::EncodeSelectionKey({ *Arcane::Guid::FromString(M), {}, {}, {} }) == M + "///");
    CHECK(Arcane::Editor::EncodeSelectionKey({}).empty());
}

TEST_CASE("input document: PageFor and Resolves agree on every key shape", "[editor][input][inspector]")
{
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / ("key-agree-" + Arcane::Guid::Generate().ToString() + ".arcinput");
    { std::ofstream out(path); out << DocumentJson().dump(2); }
    {
        auto doc = Arcane::Editor::InputActionsDocument::Open(path);
        REQUIRE(doc);
        const std::string M = "22222222-2222-4222-8222-222222222222", A = "33333333-3333-4333-8333-333333333333",
                          B = "44444444-4444-4444-8444-444444444444";
        for (const std::string key : { std::string("///"), "/" + A + "//", M + "////", M + "/" + A + "/" + B + "/junk", M + "//" + B + "/" })
        {
            INFO(key);
            CHECK(doc->PageFor(key) == nullptr);
            CHECK_FALSE(doc->Resolves(key));
        }
        CHECK(doc->PageFor("") != nullptr);          // the asset root: the one intended asymmetry
        CHECK_FALSE(doc->Resolves(""));
        REQUIRE(doc->SelectByPath("Player/Jump/0"));
        for (const auto& crumb : doc->Page()->Breadcrumb())
        {
            if (!crumb.key || crumb.key->empty()) continue;
            INFO(*crumb.key);
            CHECK(doc->PageFor(*crumb.key) != nullptr);
            CHECK(doc->Resolves(*crumb.key));
        }
    }
    fs::remove(path);
}

TEST_CASE("input editor: RestoreSelectionOrAncestor treats a malformed key as no selection", "[editor][input]")
{
    Arcane::Editor::InputActionsEditorModel model(DocumentJson());
    const std::string M = "22222222-2222-4222-8222-222222222222", A = "33333333-3333-4333-8333-333333333333";
    model.SelectMap(*Arcane::Guid::FromString(M));
    model.SelectAction(*Arcane::Guid::FromString(A));
    const auto epoch = model.SelectionEpoch();
    model.RestoreSelectionOrAncestor(M + "/bogus//");
    CHECK(model.SelectionKey().empty());
    model.RestoreSelectionOrAncestor(M + "/" + A + "/" + Arcane::Guid::Generate().ToString() + "/");   // a deleted binding
    CHECK(model.SelectedAction().ToString() == A);                                                     // falls back to its action
    CHECK_FALSE(model.SelectedBinding().IsValid());
    CHECK(model.SelectionEpoch() == epoch);                                                            // silent throughout
}

namespace
{
    // DocumentJson plus one conflict (Crouch's ungrouped space) and one unknown path (Fire's 'spaec').
    nlohmann::json WarningsJson()
    {
        auto json = DocumentJson();
        auto& actions = json["actionMaps"][0]["actions"];
        actions.push_back(nlohmann::json::parse(R"JSON({"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","name":"Crouch","type":"Button",
            "bindings":[{"id":"dddddddd-dddd-4ddd-8ddd-dddddddddddd","path":"<Keyboard>/space"}]})JSON"));
        actions.push_back(nlohmann::json::parse(R"JSON({"id":"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee","name":"Fire","type":"Button",
            "bindings":[{"id":"12121212-1212-4121-8121-121212121212","path":"<Keyboard>/spaec"}]})JSON"));
        return json;
    }
}

TEST_CASE("input editor: DraftRevision bumps on every draft change and never on selection or save", "[editor][input]")
{
    Arcane::Runtime runtime(Arcane::Test::Process());
    Arcane::CommandStack commands([&]() -> Astra::Registry& { return runtime.Registry(); });
    Arcane::Editor::InputActionsEditorModel model(DocumentJson(), &commands);
    const auto r0 = model.DraftRevision();
    model.SelectMap(*Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222"));
    model.SelectAction(*Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333"));
    CHECK(model.DraftRevision() == r0);
    REQUIRE(model.SetField(*Arcane::Guid::FromString("44444444-4444-4444-8444-444444444444"), "path", "<Keyboard>/k"));
    const auto r1 = model.DraftRevision();
    CHECK(r1 > r0);
    const auto file = std::filesystem::temp_directory_path() / ("rev-" + Arcane::Guid::Generate().ToString() + ".arcinput");
    REQUIRE(model.Save(file));
    CHECK(model.DraftRevision() == r1);
    REQUIRE(model.Undo());
    CHECK(model.DraftRevision() > r1);
    const auto r2 = model.DraftRevision();
    REQUIRE(model.Redo());
    CHECK(model.DraftRevision() > r2);
    std::filesystem::remove(file);
}

TEST_CASE("input document: Tick publishes its Warnings as asset rows with no draw, keeps them over a save, retracts a fixed one, clears on close", "[editor][input][diagnostics]")
{
    namespace fs = std::filesystem;
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();
    const auto path = fs::temp_directory_path() / ("problems-" + Arcane::Guid::Generate().ToString() + ".arcinput");
    { std::ofstream out(path); out << WarningsJson().dump(2); }
    {
        auto doc = Arcane::Editor::InputActionsDocument::Open(path);
        REQUIRE(doc);
        CHECK(doc->DiagnosticKey() == "input:11111111-1111-4111-8111-111111111111");
        const auto w = doc->Model().Warnings();
        REQUIRE(w.size() >= 2);
        doc->Tick(0.0);                                                   // no ImGui frame, no Draw
        auto rows = store.Snapshot();
        REQUIRE(rows.size() == w.size());
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            CHECK(rows[i].message == w[i]);
            CHECK(rows[i].severity == Arcane::DiagSeverity::Warning);
            CHECK(rows[i].scope == Arcane::DiagScope::Assets);
            CHECK(rows[i].locator.kind == Arcane::DiagLocator::Kind::Asset);
            CHECK(rows[i].locator.asset == doc->AssetGuid());
            CHECK(rows[i].detail == path.stem().string() + ".arcinput");
        }
        CHECK(std::any_of(rows.begin(), rows.end(), [](const auto& d) { return d.code == "input.path.unknown"; }));
        CHECK(std::any_of(rows.begin(), rows.end(), [](const auto& d) { return d.code == "input.binding.conflict"; }));
        const auto n0 = rows.size();
        REQUIRE(doc->Save());                                             // a save changes no draft: rows persist
        doc->Tick(0.0);
        CHECK(store.Snapshot().size() == n0);
        REQUIRE(doc->Model().SetField(*Arcane::Guid::FromString("12121212-1212-4121-8121-121212121212"), "path", "<Keyboard>/k"));
        doc->Tick(0.0);                                                   // as a hidden tab's page edit would be picked up
        rows = store.Snapshot();
        CHECK(rows.size() == n0 - 1);
        CHECK_FALSE(std::any_of(rows.begin(), rows.end(), [](const auto& d) { return d.code == "input.path.unknown"; }));
    }
    CHECK(store.Snapshot().empty());                                      // closing clears
    store.UninstallEngineSink();
    fs::remove(path);
}

TEST_CASE("input editor: a captured Keypad + path is not an unknown control and conflicts by compiled key", "[editor][input]")
{
    auto json = DocumentJson();
    json["actionMaps"][0]["actions"][0]["bindings"][0]["path"] = "<Keyboard>/scancode/keypad +";
    json["actionMaps"][0]["actions"].push_back(nlohmann::json::parse(R"JSON({"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","name":"Crouch","type":"Button",
        "bindings":[{"id":"dddddddd-dddd-4ddd-8ddd-dddddddddddd","path":"<Keyboard>/scancode/keypad +"}]})JSON"));
    Arcane::Editor::InputActionsEditorModel model(std::move(json));
    const auto w = model.Warnings();
    CHECK(std::none_of(w.begin(), w.end(), [](const std::string& s) { return s.starts_with("Unknown control path"); }));
    CHECK(std::count_if(w.begin(), w.end(), [](const std::string& s) { return s.starts_with("Conflicting"); }) == 1);
}

TEST_CASE("input document: a refused SelectByPath leaves the opening selection and epoch untouched", "[editor][input][inspector]")
{
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / ("select-refused-" + Arcane::Guid::Generate().ToString() + ".arcinput");
    { std::ofstream out(path); out << DocumentJson().dump(2); }
    {
        auto doc = Arcane::Editor::InputActionsDocument::Open(path);
        REQUIRE(doc);
        const std::string key = doc->SelectionKey();   // opening selection: first map + first action
        const auto epoch = doc->SelectionEpoch();
        Arcane::Editor::EditorDocument& base = *doc;    // the host's virtual dispatch
        CHECK_FALSE(base.SelectByPath("Player/NoSuchAction"));
        CHECK_FALSE(base.SelectByPath("Player/Jump/7"));
        CHECK(doc->SelectionKey() == key);
        CHECK(doc->SelectionEpoch() == epoch);
    }
    fs::remove(path);
}
