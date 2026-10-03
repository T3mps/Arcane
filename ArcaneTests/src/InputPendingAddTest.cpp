// Add-and-listen (spec 2026-09-30 s8.3): the pending add is pure data; the
// draft is touched only by CommitPending's ONE model call.
#include <catch2/catch_test_macros.hpp>
#include "Documents/InputPendingAdd.hpp"
#include "Documents/InputActionsEditorModel.hpp"
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Base/Runtime.hpp>
#include "Helpers/TestTypeContext.hpp"

namespace
{
    using namespace Arcane::Editor;
    using Arcane::Guid;
    Guid G(const char* s) { return *Guid::FromString(s); }
    constexpr const char* kMap = "22222222-2222-4222-8222-222222222222";
    constexpr const char* kJump = "33333333-3333-4333-8333-333333333333";
    constexpr const char* kJumpBinding = "44444444-4444-4444-8444-444444444444";
    constexpr const char* kMove = "88888888-8888-4888-8888-888888888888";
    constexpr const char* kMoveComposite = "99999999-9999-4999-8999-999999999999";
    constexpr const char* kUp = "a0000001-0000-4000-8000-000000000001";
    constexpr const char* kDown = "a0000002-0000-4000-8000-000000000002";
    constexpr const char* kLeft = "a0000003-0000-4000-8000-000000000003";
    constexpr const char* kRight = "a0000004-0000-4000-8000-000000000004";
    nlohmann::json Doc()
    {
        return nlohmann::json::parse(R"JSON({
            "version":1,"id":"11111111-1111-4111-8111-111111111111","defaultMap":"22222222-2222-4222-8222-222222222222",
            "controlSchemes":[{"id":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","name":"Keyboard and Mouse","bindingGroup":"KeyboardMouse"}],
            "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"Player","actions":[
              {"id":"33333333-3333-4333-8333-333333333333","name":"Jump","type":"Button","bindings":[
                {"id":"44444444-4444-4444-8444-444444444444","path":"<Keyboard>/space"}]},
              {"id":"88888888-8888-4888-8888-888888888888","name":"Move","type":"Axis2D","bindings":[
                {"id":"99999999-9999-4999-8999-999999999999","composite":"2DVector","groups":["KeyboardMouse"],"parts":[
                  {"id":"a0000001-0000-4000-8000-000000000001","name":"up","path":"<Keyboard>/i"},
                  {"id":"a0000002-0000-4000-8000-000000000002","name":"down","path":"<Keyboard>/k"},
                  {"id":"a0000003-0000-4000-8000-000000000003","name":"left","path":"<Keyboard>/j"},
                  {"id":"a0000004-0000-4000-8000-000000000004","name":"right","path":"<Keyboard>/l"}]}]}]}]})JSON");
    }
    struct Rig
    {
        Arcane::Runtime runtime{ Arcane::Test::Process() };
        Arcane::CommandStack commands{ [this]() -> Astra::Registry& { return runtime.Registry(); } };
    };
    const nlohmann::json& Bindings(const InputActionsEditorModel& m, int action) { return m.Draft()["actionMaps"][0]["actions"][action]["bindings"]; }
}

TEST_CASE("pending add: role orders -- 1DAxis negative,positive; 2DVector up,down,left,right; a part its one role; a rebind the draft's part order", "[editor][input]")
{
    CHECK(MakeAddComposite(G(kMap), G(kJump), "1DAxis", {}).roles == std::vector<std::string>{ "negative", "positive" });
    CHECK(MakeAddComposite(G(kMap), G(kJump), "2DVector", {}).roles == std::vector<std::string>{ "up", "down", "left", "right" });
    CHECK(MakeAddBinding(G(kMap), G(kJump), {}).roles.size() == 1);
    CHECK(MakeAddPart(G(kMoveComposite), "left").roles == std::vector<std::string>{ "left" });
    const auto rebind = MakeRebindComposite(Doc(), G(kMoveComposite));
    REQUIRE(rebind);
    CHECK(rebind->kind == PendingAdd::Kind::RebindComposite);
    CHECK(rebind->roles == std::vector<std::string>{ "up", "down", "left", "right" });
    CHECK(rebind->parts == std::vector<Guid>{ G(kUp), G(kDown), G(kLeft), G(kRight) });
    CHECK(rebind->map == G(kMap));
    CHECK(rebind->action == G(kMove));
    CHECK_FALSE(MakeRebindComposite(Doc(), G(kJumpBinding)));   // a simple binding is not a composite
}

TEST_CASE("pending add: committing with nothing captured adds nothing and pushes no undo step", "[editor][input]")
{
    Rig rig;
    InputActionsEditorModel model(Doc(), [&rig]() -> Arcane::CommandStack* { return &rig.commands; });
    const nlohmann::json before = model.Draft();
    for (const PendingAdd& p : { MakeAddBinding(G(kMap), G(kJump), {}), MakeAddComposite(G(kMap), G(kJump), "2DVector", {}),
                                 MakeAddPart(G(kMoveComposite), "up"), *MakeRebindComposite(Doc(), G(kMoveComposite)) })
    {
        CHECK_FALSE(p.Done());
        CHECK_FALSE(CommitPending(model, p));
    }
    CHECK(model.Draft() == before);
    CHECK_FALSE(rig.commands.CanUndo());
}

TEST_CASE("pending add: 2 of 4 captured gives a composite holding only up and down, in one step", "[editor][input]")
{
    Rig rig;
    InputActionsEditorModel model(Doc(), [&rig]() -> Arcane::CommandStack* { return &rig.commands; });
    PendingAdd p = MakeAddComposite(G(kMap), G(kJump), "2DVector", { "KeyboardMouse" });
    p.captured = { "<Keyboard>/w", "<Keyboard>/s" };
    CHECK_FALSE(p.Done());
    REQUIRE(CommitPending(model, p));
    const nlohmann::json composite = Bindings(model, 0)[1];
    REQUIRE(composite["parts"].size() == 2);
    CHECK(composite["parts"][0]["name"] == "up");
    CHECK(composite["parts"][1]["name"] == "down");
    CHECK(composite["groups"] == nlohmann::json::array({ "KeyboardMouse" }));
    CHECK(std::string(rig.commands.UndoLabel()) == "Add composite binding");
    REQUIRE(model.Undo());
    CHECK_FALSE(rig.commands.CanUndo());
}

TEST_CASE("pending add: a whole-composite rebind keeps the part ids and re-paths only what was heard", "[editor][input]")
{
    Rig rig;
    InputActionsEditorModel model(Doc(), [&rig]() -> Arcane::CommandStack* { return &rig.commands; });
    PendingAdd p = *MakeRebindComposite(Doc(), G(kMoveComposite));
    p.captured = { "<Keyboard>/w", "<Keyboard>/s" };
    REQUIRE(CommitPending(model, p));
    const nlohmann::json parts = Bindings(model, 1)[0]["parts"];
    REQUIRE(parts.size() == 4);
    CHECK(parts[0]["id"] == kUp);   CHECK(parts[0]["path"] == "<Keyboard>/w");
    CHECK(parts[1]["id"] == kDown); CHECK(parts[1]["path"] == "<Keyboard>/s");
    CHECK(parts[2]["path"] == "<Keyboard>/j");
    CHECK(parts[3]["path"] == "<Keyboard>/l");
    CHECK(std::string(rig.commands.UndoLabel()) == "Rebind composite");
}

TEST_CASE("pending add: a part commits ONE AddPart with its role and the heard path", "[editor][input]")
{
    Rig rig;
    InputActionsEditorModel model(Doc(), [&rig]() -> Arcane::CommandStack* { return &rig.commands; });
    PendingAdd p = MakeAddPart(G(kMoveComposite), "left");
    p.captured = { "<Keyboard>/a" };
    CHECK(p.Done());
    REQUIRE(CommitPending(model, p));
    const nlohmann::json parts = Bindings(model, 1)[0]["parts"];
    REQUIRE(parts.size() == 5);
    CHECK(parts[4]["name"] == "left");
    CHECK(parts[4]["path"] == "<Keyboard>/a");
    CHECK(std::string(rig.commands.UndoLabel()) == "Add composite part");
    REQUIRE(model.Undo());
    CHECK_FALSE(rig.commands.CanUndo());
}

TEST_CASE("pending add: a binding commits with its groups; PrefillGroups uses only a filter some scheme names", "[editor][input]")
{
    InputActionsEditorModel model(Doc());
    PendingAdd p = MakeAddBinding(G(kMap), G(kJump), PrefillGroups(Doc(), "KeyboardMouse"));
    p.captured = { "<Keyboard>/w" };
    CHECK(p.Done());
    REQUIRE(CommitPending(model, p));
    CHECK(Bindings(model, 0)[1]["groups"] == nlohmann::json::array({ "KeyboardMouse" }));
    CHECK(PrefillGroups(Doc(), "").empty());          // All schemes: ungrouped
    CHECK(PrefillGroups(Doc(), "Gamepad").empty());   // a stale filter: ungrouped
}
