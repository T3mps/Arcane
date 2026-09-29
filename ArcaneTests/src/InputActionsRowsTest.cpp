// InputActionsRows (input-editor redesign spec s2.3): the PURE row list the
// actions column draws -- expanded bindings, composite header + parts,
// readable names, scheme badges, the search and scheme filters, collapse,
// keyboard stepping. No ImGui.
#include <catch2/catch_test_macros.hpp>
#include <Documents/InputActionsRows.hpp>
#include <Documents/InputActionsEditorModel.hpp>
#include <unordered_set>

using namespace Arcane::Editor;

namespace
{
    nlohmann::json Fixture()
    {
        return nlohmann::json::parse(R"JSON({
          "version":1,"id":"11111111-1111-4111-8111-111111111111",
          "controlSchemes":[{"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","name":"KeyboardMouse","bindingGroup":"KeyboardMouse"},
                            {"id":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","name":"Gamepad","bindingGroup":"Gamepad"}],
          "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"Player","actions":[
            {"id":"33333333-3333-4333-8333-333333333333","name":"Move","type":"Axis1D","bindings":[
              {"id":"44444444-4444-4444-8444-444444444444","composite":"1DAxis","groups":["KeyboardMouse"],"parts":[
                {"id":"55555555-5555-4555-8555-555555555555","name":"negative","path":"<Keyboard>/scancode/a"},
                {"id":"66666666-6666-4666-8666-666666666666","name":"positive","path":"<Keyboard>/scancode/d"}]},
              {"id":"77777777-7777-4777-8777-777777777777","path":"<Gamepad>/leftStick/x","groups":["Gamepad"]}]},
            {"id":"88888888-8888-4888-8888-888888888888","name":"Jump","type":"Button","interactions":["hold(duration=0.3)"],"bindings":[
              {"id":"99999999-9999-4999-8999-999999999999","path":"<Keyboard>/space","groups":["KeyboardMouse"]},
              {"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","path":"<Gamepad>/buttonSouth"}]}
          ]}]})JSON");
    }
    const Arcane::Guid kMap = *Arcane::Guid::FromString("22222222-2222-4222-8222-222222222222");
}

TEST_CASE("input rows: expanded bindings, composite header + parts, readable names, badges", "[editor][input]")
{
    const auto rows = BuildInputRows(Fixture(), kMap, {}, {}, {});
    REQUIRE(rows.size() == 10);
    CHECK(rows[0].kind == InputRowKind::Action);          CHECK(rows[0].name == "Move");   CHECK(rows[0].badge == "Axis1D"); CHECK(rows[0].detail.empty());
    CHECK(rows[1].kind == InputRowKind::CompositeHeader); CHECK(rows[1].name == "1D Axis"); CHECK(rows[1].badge == "KeyboardMouse"); CHECK(rows[1].depth == 1);
    CHECK(rows[2].kind == InputRowKind::Part);            CHECK(rows[2].name == "A");      CHECK(rows[2].detail == "1D Axis · negative"); CHECK(rows[2].device == "Keyboard"); CHECK(rows[2].depth == 2);
    CHECK(rows[2].bindingId.ToString() == "44444444-4444-4444-8444-444444444444");
    CHECK(rows[3].kind == InputRowKind::Part);            CHECK(rows[3].name == "D");
    CHECK(rows[4].kind == InputRowKind::Binding);         CHECK(rows[4].name == "Left Stick X"); CHECK(rows[4].device == "Gamepad"); CHECK(rows[4].badge == "Gamepad");
    CHECK(rows[5].kind == InputRowKind::AddBinding);      CHECK(rows[5].actionId.ToString() == "33333333-3333-4333-8333-333333333333");
    CHECK(rows[6].kind == InputRowKind::Action);          CHECK(rows[6].name == "Jump");   CHECK(rows[6].badge == "Button"); CHECK(rows[6].detail == "Hold 0.30 s");
    CHECK(rows[7].name == "Space");                       CHECK(rows[7].path == "<Keyboard>/space");
    CHECK(rows[8].name == "South Button");                CHECK(rows[8].badge.empty());   // ungrouped: no scheme badge
    CHECK(rows[9].kind == InputRowKind::AddBinding);
}

TEST_CASE("input rows: scheme filter hides other groups, ungrouped always shows; search narrows", "[editor][input]")
{
    InputRowFilter f; f.schemeGroup = "Gamepad";
    auto rows = BuildInputRows(Fixture(), kMap, f, {}, {});
    std::vector<std::string> names; for (const auto& r : rows) names.push_back(r.name);
    CHECK(names == std::vector<std::string>{ "Move", "Left Stick X", "", "Jump", "South Button", "" });
    f = {}; f.search = "SPACE";
    rows = BuildInputRows(Fixture(), kMap, f, {}, {});
    names.clear(); for (const auto& r : rows) names.push_back(r.name);
    CHECK(names == std::vector<std::string>{ "Jump", "Space", "" });
    f = {}; f.search = "mov";                                          // action name hit: every binding shows
    rows = BuildInputRows(Fixture(), kMap, f, {}, {});
    CHECK(rows.size() == 6);
    CHECK(rows[0].name == "Move");
}

TEST_CASE("input rows: collapse, conflicts, malformed drafts", "[editor][input]")
{
    std::unordered_set<std::string> collapsed{ "33333333-3333-4333-8333-333333333333" };
    auto rows = BuildInputRows(Fixture(), kMap, {}, {}, collapsed);
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].name == "Move");
    CHECK(rows[1].name == "Jump");
    InputRowFilter f; f.search = "stick";                                  // search overrides collapse
    rows = BuildInputRows(Fixture(), kMap, f, {}, collapsed);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].name == "Move"); CHECK(rows[1].name == "Left Stick X"); CHECK(rows[2].kind == InputRowKind::AddBinding);
    rows = BuildInputRows(Fixture(), kMap, {}, {}, collapsed);             // search cleared: the collapse returns
    CHECK(rows.size() == 5);
    std::vector<InputActionsEditorModel::BindingConflict> conflicts;
    conflicts.push_back({ *Arcane::Guid::FromString("99999999-9999-4999-8999-999999999999"), {}, {}, "Crouch", "<Keyboard>/space", "KeyboardMouse" });
    rows = BuildInputRows(Fixture(), kMap, {}, conflicts, {});
    CHECK(rows[7].conflict);
    CHECK_FALSE(rows[8].conflict);
    CHECK(BuildInputRows(nlohmann::json("not an object"), kMap, {}, {}, {}).empty());
    CHECK(BuildInputRows(nlohmann::json::object(), kMap, {}, {}, {}).empty());
    CHECK(BuildInputRows(Fixture(), Arcane::Guid::Generate(), {}, {}, {}).empty());
}

TEST_CASE("input rows: StepSelection walks selectable rows and skips the ghost rows", "[editor][input]")
{
    const auto rows = BuildInputRows(Fixture(), kMap, {}, {}, {});
    const auto stick = *Arcane::Guid::FromString("77777777-7777-4777-8777-777777777777");
    auto next = StepSelection(rows, stick, +1);
    REQUIRE(next); CHECK(next->ToString() == "88888888-8888-4888-8888-888888888888");   // Jump, not the AddBinding ghost
    auto prev = StepSelection(rows, stick, -1);
    REQUIRE(prev); CHECK(prev->ToString() == "66666666-6666-4666-8666-666666666666");
    CHECK(StepSelection(rows, *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333"), -1)->ToString() == "33333333-3333-4333-8333-333333333333");   // clamps at the top
    CHECK(StepSelection(rows, Arcane::Guid::Generate(), +1)->ToString() == "33333333-3333-4333-8333-333333333333");   // unknown: the first row
    CHECK_FALSE(StepSelection({}, stick, +1));
}

TEST_CASE("input rows: InteractionText and SiblingIndex", "[editor][input]")
{
    CHECK(InteractionText(nlohmann::json::array()) == "");
    CHECK(InteractionText(nlohmann::json::array({ "press" })) == "Press");
    CHECK(InteractionText(nlohmann::json::array({ "hold" })) == "Hold 0.40 s");
    CHECK(InteractionText(nlohmann::json::array({ "hold(duration=0.3)" })) == "Hold 0.30 s");
    CHECK(InteractionText(nlohmann::json::array({ "tap(duration=0.15)", "press" })) == "Tap 0.15 s · Press");
    CHECK(InteractionText(nlohmann::json("nope")) == "");
    const auto pos = SiblingIndex(Fixture(), *Arcane::Guid::FromString("cccccccc-cccc-4ccc-8ccc-cccccccccccc"));
    REQUIRE(pos);
    CHECK(pos->parent.ToString() == "88888888-8888-4888-8888-888888888888");
    CHECK(pos->index == 1); CHECK(pos->count == 2);
    const auto part = SiblingIndex(Fixture(), *Arcane::Guid::FromString("66666666-6666-4666-8666-666666666666"));
    REQUIRE(part); CHECK(part->parent.ToString() == "44444444-4444-4444-8444-444444444444"); CHECK(part->index == 1);
    CHECK_FALSE(SiblingIndex(Fixture(), Arcane::Guid::Generate()));
}

TEST_CASE("input rows: a binding in several schemes carries every group", "[editor][input]")
{
    const auto draft = nlohmann::json::parse(R"JSON({"version":1,"id":"11111111-1111-4111-8111-111111111111","controlSchemes":[],
      "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"P","actions":[
        {"id":"33333333-3333-4333-8333-333333333333","name":"Fire","type":"Button","bindings":[
          {"id":"44444444-4444-4444-8444-444444444444","path":"<Keyboard>/f","groups":["KeyboardMouse","Gamepad"]}]}]}]})JSON");
    const auto rows = BuildInputRows(draft, kMap, {}, {}, {});
    REQUIRE(rows.size() == 3);
    CHECK(rows[1].badge == "KeyboardMouse, Gamepad");
    REQUIRE(rows[1].groups.size() == 2);
    CHECK(rows[1].groups[1] == "Gamepad");
}

TEST_CASE("input rows: ConflictTooltip names each other action once with its schemes", "[editor][input]")
{
    using C = InputActionsEditorModel::BindingConflict;
    const auto a = *Arcane::Guid::FromString("99999999-9999-4999-8999-999999999999");
    const auto b = *Arcane::Guid::FromString("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    std::vector<C> cs;
    C c1{}; c1.binding = a; c1.otherBinding = b; c1.otherActionName = "Crouch"; c1.group = "KeyboardMouse"; c1.scheme = "KeyboardMouse";
    C c2 = c1;                                   // the same other action again: named once
    C c3{}; c3.binding = a; c3.otherActionName = "Fire"; c3.group = "*";
    cs = { c1, c2, c3 };
    CHECK(ConflictTooltip(cs, a) == "Also bound by Crouch (KeyboardMouse), Fire (every scheme)");
    CHECK(ConflictTooltip(cs, b).empty());
}
