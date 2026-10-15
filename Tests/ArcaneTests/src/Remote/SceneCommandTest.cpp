#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Commands/SceneCommands.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Remote/Worlds.hpp>
#include <Arcane/Scene/Components.hpp>
#include "Scene/SelectionContext.hpp"
#include "Remote/SelectionCommand.hpp"
#include <Astra/Registry/Registry.hpp>
#include <Servitor/Verbs.hpp>
#include "RemoteTestSupport.hpp"
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

// The one-row world table every world-taking command reads (Task 19, ruling F16).
static std::vector<Arcane::Remote::WorldInfo> RemoteTestWorld(void* user) {
    Arcane::Remote::WorldInfo world;
    world.name = "main";
    world.role = "Standalone";
    world.registry = static_cast<Astra::Registry*>(user);
    return {world};
}

static Arcane::Editor::SelectionContext* g_sel = nullptr;
static std::size_t SelCount(void*) { return g_sel ? g_sel->Count() : 0; }
static Astra::Entity SelAt(std::size_t i, void*) { return g_sel->Entities()[i]; }
static Astra::Entity SelPrimary(void*) {
    return g_sel ? g_sel->Primary() : Astra::Entity::Invalid();
}

TEST_CASE("scene.tree roots are parentless and siblings sort by guid", "[remote][scene]") {
    Astra::Registry reg;
    const auto first = reg.CreateEntity();
    const auto second = reg.CreateEntity();
    const auto child = reg.CreateEntity();
    REQUIRE(reg.SetParent(child, second));
    Arcane::Identity a;
    a.id = Arcane::Guid{1, 9};
    a.name = "later";
    Arcane::Identity b;
    b.id = Arcane::Guid{1, 2};
    b.name = "earlier";
    Arcane::Identity c;
    c.id = Arcane::Guid{1, 3};
    c.name = "child";
    reg.AddComponent<Arcane::Identity>(first, a);
    reg.AddComponent<Arcane::Identity>(second, b);
    reg.AddComponent<Arcane::Identity>(child, c);
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};   // one-row table (ruling F16)
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterSceneCommands();
    RemoteTest::ScopedHostKind editorKind(Arcane::Remote::HostKind::Editor);
    Arcane::Remote::Call call;
    call.command = "scene.tree";
    call.args = nlohmann::json::object();
    const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK(env.ok);
    REQUIRE(env.data["roots"].size() == 2);
    CHECK(env.data["roots"][0]["name"] == "earlier");
    CHECK(env.data["roots"][1]["name"] == "later");
    REQUIRE(env.data["roots"][0]["children"].size() == 1);
    CHECK(env.data["roots"][0]["children"][0]["name"] == "child");
}

TEST_CASE("scene.tree on a server is command.host-kind", "[remote][scene]") {
    Astra::Registry reg;
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};   // one-row table (ruling F16)
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterSceneCommands();
    RemoteTest::ScopedHostKind serverKind(Arcane::Remote::HostKind::Server);   // ruling F8
    Arcane::Remote::Call call;
    call.command = "scene.tree";
    const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK_FALSE(env.ok);
    CHECK(env.errors[0].code == "command.host-kind");
    CHECK(Arcane::Remote::ExitFromHost(env) == 2);
}

TEST_CASE("selection.get keeps user order and a null store is permission.denied", "[remote][scene]") {
    Astra::Registry reg;
    const auto e1 = reg.CreateEntity();
    const auto e2 = reg.CreateEntity();
    Arcane::Identity a;
    a.id = Arcane::Guid{2, 1};
    a.name = "first";
    Arcane::Identity b;
    b.id = Arcane::Guid{2, 2};
    b.name = "second";
    reg.AddComponent<Arcane::Identity>(e1, a);
    reg.AddComponent<Arcane::Identity>(e2, b);
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};   // one-row table (ruling F16)
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Editor::SelectionContext sel;
    sel.Select(e1);
    sel.Toggle(e2);
    g_sel = &sel;
    Arcane::Editor::SelectionView view;
    view.count = &SelCount;
    view.at = &SelAt;
    view.primary = &SelPrimary;
    // Cleared on scope exit even if a REQUIRE aborts (ruling S3-9).
    RemoteTest::ScopedRemotePointer<const Arcane::Editor::SelectionView> viewGuard(
        [](const Arcane::Editor::SelectionView* v) {
            Arcane::Editor::SetSelectionView(v ? *v : Arcane::Editor::SelectionView{});
        },
        &view);
    Arcane::Editor::RegisterSelectionCommand();
    Arcane::Remote::Call call;
    call.command = "editor.selection.get";
    const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK(env.ok);
    REQUIRE(env.data["entities"].size() == 2);
    CHECK(env.data["entities"][0]["name"] == "first");
    CHECK(env.data["entities"][1]["name"] == "second");
    CHECK(env.data["primary"]["lo"] == 2);
    g_sel = nullptr;
    Arcane::Editor::SetSelectionView({});
    const auto denied = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK_FALSE(denied.ok);
    CHECK(denied.errors[0].code == "permission.denied");
    CHECK(Arcane::Remote::ExitFromHost(denied) == 3);
}

TEST_CASE("scene.tree --depth 0 is roots only and an entity without Identity sorts last", "[remote][scene]") {
    Astra::Registry reg;
    const auto anonymous = reg.CreateEntity();   // created first, still sorts last
    const auto named = reg.CreateEntity();
    const auto child = reg.CreateEntity();
    REQUIRE(reg.SetParent(child, named));
    Arcane::Identity n;
    n.id = Arcane::Guid{5, 5};
    n.name = "named";
    reg.AddComponent<Arcane::Identity>(named, n);
    Arcane::Identity c;
    c.id = Arcane::Guid{5, 6};
    c.name = "kid";
    reg.AddComponent<Arcane::Identity>(child, c);
    Arcane::Remote::WorldTable worlds{&RemoteTestWorld, &reg};
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::WorldTable> worldGuard(
        [](const Arcane::Remote::WorldTable* t) { Arcane::Remote::SetCommandWorlds(t ? *t : Arcane::Remote::WorldTable{}); },
        &worlds);
    Arcane::Remote::RegisterSceneCommands();
    RemoteTest::ScopedHostKind runtimeKind(Arcane::Remote::HostKind::Runtime);
    Arcane::Remote::Call call;
    call.command = "scene.tree";

    // The whole tree: the anonymous root is last, guid null, handle kept, one warning.
    auto env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    REQUIRE(env.ok);
    REQUIRE(env.data["roots"].size() == 2);
    CHECK(env.data["roots"][0]["name"] == "named");
    CHECK(env.data["roots"][0]["ref"] == "guid:" + n.id.ToString());
    CHECK(env.data["roots"][0]["guid"]["hi"] == 5);
    CHECK(env.data["roots"][1]["guid"].is_null());
    CHECK(env.data["roots"][1]["handle"] == static_cast<std::uint64_t>(anonymous.GetValue()));
    REQUIRE(env.warnings.size() == 1);
    CHECK(env.warnings[0].code == "entity.no-identity");
    REQUIRE(env.data["roots"][0]["children"].size() == 1);

    // depth 0: roots only; the cut node says how many children it had.
    call.args = {{"depth", 0}};
    env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    REQUIRE(env.ok);
    REQUIRE(env.data["roots"].size() == 2);
    CHECK(env.data["roots"][0]["children"].empty());
    CHECK(env.data["roots"][0]["childrenOmitted"] == 1);

    // depth -1 (the schema default) is the whole tree.
    call.args = {{"depth", -1}};
    env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    REQUIRE(env.ok);
    REQUIRE(env.data["roots"][0]["children"].size() == 1);
    CHECK_FALSE(env.data["roots"][0].contains("childrenOmitted"));

    // Bad arguments are usage refusals, never a silent default.
    call.args = {{"depth", -2}};
    env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK_FALSE(env.ok);
    CHECK(env.errors[0].code == "arg.invalid");
    call.args = {{"deep", 1}};
    env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK_FALSE(env.ok);
    CHECK(env.errors[0].code == "arg.unknown");
    CHECK(Arcane::Remote::ExitFromHost(env) == 2);
}

// editor.selection.get on a runtime or server answers command.host-kind because
// those hosts never register it (ruling F-1); Task 4's never-registered-name
// case pins that registry rule, so it is not re-tested here (this process has
// registered it).
TEST_CASE("scene.tree and editor.selection.get are world-scoped Observe reads", "[remote][scene]") {
    Arcane::Remote::RegisterSceneCommands();
    Arcane::Editor::RegisterSelectionCommand();
    Arcane::Editor::RegisterSelectionCommand();   // safe to call twice
    const auto* tree = Arcane::Remote::CommandRegistry::Get().Find("scene.tree");
    const auto* selection = Arcane::Remote::CommandRegistry::Get().Find("editor.selection.get");
    REQUIRE(tree != nullptr);
    REQUIRE(selection != nullptr);
    CHECK(tree->worldScoped);
    CHECK(selection->worldScoped);
    CHECK(tree->access == Arcane::Remote::Access::Observe);
    CHECK(selection->access == Arcane::Remote::Access::Observe);
    CHECK(tree->mutates == Arcane::Remote::Mutates::None);
    CHECK(selection->mutates == Arcane::Remote::Mutates::None);
    CHECK_FALSE(tree->needsFrame);
    CHECK_FALSE(selection->needsFrame);
    CHECK(tree->module.empty());
    CHECK(selection->module.empty());
}

TEST_CASE("scene and selection verbs build their wire args", "[remote][scene][servitor]") {
    const Servitor::VerbRow* tree = Servitor::FindVerb("scene tree");
    const Servitor::VerbRow* selection = Servitor::FindVerb("selection");
    REQUIRE(tree != nullptr);
    REQUIRE(selection != nullptr);
    CHECK(std::string(tree->command) == "scene.tree");
    CHECK(std::string(selection->command) == "editor.selection.get");
    CHECK(selection->args == nullptr);   // Run refuses any token after `selection`
    nlohmann::json out;
    std::string error;
    std::vector<std::string> none;
    REQUIRE(tree->args(none, out, &error));
    CHECK(out == nlohmann::json::object());
    std::vector<std::string> depth{"--depth", "2"};
    REQUIRE(tree->args(depth, out, &error));
    CHECK(out == nlohmann::json{{"depth", 2}});
    for (const auto& bad : std::vector<std::vector<std::string>>{
             {"--depth", "x"}, {"--depth", "-1"}, {"--bogus"}, {"extra"}}) {
        error.clear();
        CHECK_FALSE(tree->args(bad, out, &error));
        CHECK_FALSE(error.empty());
    }
}

#endif
