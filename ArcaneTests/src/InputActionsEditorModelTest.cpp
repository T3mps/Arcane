#include <catch2/catch_test_macros.hpp>

#include "Documents/InputActionsEditorModel.hpp"
#include "Documents/InputActionsDocument.hpp"
#include "Documents/DocumentHost.hpp"

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Base/Runtime.hpp>

#include "Helpers/TestTypeContext.hpp"

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
