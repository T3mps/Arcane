#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/ConsoleModel.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Render/Nri/nodes/MeshCullNode.hpp>
#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <cstdint>
#include <filesystem>
#include <limits>

using namespace Arcane;

TEST_CASE("cvar values round-trip and clamp", "[cvar]") {
    const CVarValue wide = CVarValue::UInt32(0xffffffffu);
    REQUIRE(wide.type == CVarType::UInt32);
    REQUIRE(wide.AsUInt32() == 0xffffffffu);
    const CVarValue bigger = CVarValue::UInt64(std::numeric_limits<std::uint64_t>::max());
    REQUIRE(bigger.AsUInt64() == std::numeric_limits<std::uint64_t>::max());

    const CVarValue clamped = Clamp(CVarValue::Int32(50), CVarValue::Int32(0), CVarValue::Int32(10));
    REQUIRE(clamped.AsInt32() == 10);
    const CVarValue lifted = Clamp(CVarValue::Int32(-3), CVarValue::Int32(0), CVarValue::Int32(10));
    REQUIRE(lifted.AsInt32() == 0);
    REQUIRE(Clamp(CVarValue::String("nope"), CVarValue::Int32(0), CVarValue::Int32(1)).AsString() == "nope");
}

TEST_CASE("cvar registry owns the value and kills stale handles", "[cvar]") {
    CVarRegistry reg;
    const CVarHandle h = reg.Register(CVarDesc{
        "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(false),
        std::nullopt, std::nullopt, CVarFlags::Dev, "markers", "engine" });
    REQUIRE_FALSE(h.IsStale());
    REQUIRE(reg.Get(h)->AsBool() == false);

    reg.UnregisterModule("engine");
    REQUIRE_FALSE(reg.Get(h).has_value());

    const CVarHandle again = reg.Register(CVarDesc{
        "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(true),
        std::nullopt, std::nullopt, CVarFlags::None, "", "engine" });
    REQUIRE(again.index == h.index);
    REQUIRE(again.generation != h.generation);
    REQUIRE_FALSE(reg.Get(h).has_value());
    REQUIRE(reg.Get(again)->AsBool() == true);
}

TEST_CASE("duplicate cvar registration names both modules", "[cvar]") {
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(CVarDesc{ "a.b", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "", "one" }).IsStale());
    REQUIRE(reg.Register(CVarDesc{ "a.b", CVarType::Int32, CVarValue::Int32(2), {}, {}, {}, "", "two" }).IsStale());
    REQUIRE(reg.LastError().find("one") != std::string::npos);
    REQUIRE(reg.LastError().find("two") != std::string::npos);
    REQUIRE(reg.Get(reg.Find("a.b"))->AsInt32() == 1);
}

TEST_CASE("weaker SetBy cannot stomp, and module unload pops its sets", "[cvar]") {
    CVarRegistry reg;
    const CVarHandle h = reg.Register(CVarDesc{
        "render.meshCull", CVarType::Bool, CVarValue::Bool(true), {}, {}, {}, "", "engine" });
    REQUIRE(reg.Set(h, CVarValue::Bool(false), SetBy::Project, "project") == SetResult::Applied);
    REQUIRE(reg.Set(h, CVarValue::Bool(true), SetBy::EngineConfig, "engine-config") == SetResult::RefusedWeaker);
    reg.Publish();
    REQUIRE(reg.Get(h)->AsBool() == false);

    REQUIRE(reg.Set(h, CVarValue::Bool(true), SetBy::Code, "tool") == SetResult::Applied);
    REQUIRE(reg.Set(h, CVarValue::Bool(false), SetBy::Console, "user") == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Get(h)->AsBool() == false);
    REQUIRE(reg.Explain("render.meshCull")->setBy == SetBy::Console);

    reg.UnregisterModule("user");
    reg.Publish();
    REQUIRE(reg.Get(h)->AsBool() == true);
    REQUIRE(reg.Explain("render.meshCull")->setBy == SetBy::Code);
}

TEST_CASE("a set is invisible until publish, and a callback set waits", "[cvar]") {
    CVarRegistry reg;
    const CVarHandle h = reg.Register(CVarDesc{
        "debug.step", CVarType::Int32, CVarValue::Int32(0), {}, {}, CVarFlags::UserSettable, "", "engine" });
    REQUIRE(reg.Set(h, CVarValue::Int32(3), SetBy::Code) == SetResult::Applied);
    REQUIRE(reg.Get(h)->AsInt32() == 0);
    int fires = 0;
    struct State { CVarRegistry* reg; int* fires; CVarHandle handle; };
    State state{ &reg, &fires, h };
    reg.AddCallback(h, [](CVarHandle handle, void* user) {
        auto* s = static_cast<State*>(user);
        ++(*s->fires);
        s->reg->Set(handle, CVarValue::Int32(9), SetBy::Code);
    }, &state);
    reg.Publish();
    REQUIRE(fires == 1);
    REQUIRE(reg.Get(h)->AsInt32() == 3);
    reg.Publish();
    REQUIRE(fires == 2);
    REQUIRE(reg.Get(h)->AsInt32() == 9);
    reg.Publish();
    REQUIRE(fires == 2);
}

TEST_CASE("cvarlist hides Hidden and cvar_explain names the winning layer", "[cvar]") {
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(CVarDesc{ "secret.token", CVarType::String, CVarValue::String("x"), {}, {}, CVarFlags::Hidden, "", "engine" }).IsStale());
    REQUIRE_FALSE(reg.Register(CVarDesc{ "visible.knob", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "a knob", "engine" }).IsStale());
    const auto list = reg.List();
    bool sawSecret = false;
    bool sawKnob = false;
    for (const auto& e : list)
    {
        if (e.name == "secret.token") sawSecret = true;
        if (e.name == "visible.knob") sawKnob = true;
    }
    REQUIRE_FALSE(sawSecret);
    REQUIRE(sawKnob);

    const ExecResult explained = reg.Execute("cvar_explain visible.knob", Permission::Editor);
    REQUIRE(explained.ok);
    REQUIRE(explained.text.find("Default") != std::string::npos);
    REQUIRE(explained.text.find("visible.knob") != std::string::npos);
}

TEST_CASE("default-deny and cheat revert", "[cvar]") {
    CVarRegistry reg;
    const CVarHandle plain = reg.Register(CVarDesc{
        "game.speed", CVarType::Float32, CVarValue::Float32(1.f), {}, {}, {}, "", "engine" });
    REQUIRE(reg.Set(plain, CVarValue::Float32(2.f), SetBy::Console, {}, Permission::Player) == SetResult::Denied);
    REQUIRE(reg.Set(plain, CVarValue::Float32(2.f), SetBy::Console, {}, Permission::Editor) == SetResult::Applied);

    const CVarHandle cheat = reg.Register(CVarDesc{
        "game.noclip", CVarType::Bool, CVarValue::Bool(false), {}, {},
        CVarFlags::Cheat | CVarFlags::UserSettable, "", "engine" });
    REQUIRE(reg.Set(cheat, CVarValue::Bool(false), SetBy::Project, "project") == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Set(cheat, CVarValue::Bool(true), SetBy::Code, "tool", Permission::Player) == SetResult::Denied);

    const CVarHandle gate = reg.Register(CVarDesc{
        "cheats", CVarType::Bool, CVarValue::Bool(false), {}, {}, {}, "", "engine" });
    REQUIRE(reg.Set(gate, CVarValue::Bool(true), SetBy::Console, {}, Permission::Player) == SetResult::Denied);
    REQUIRE(reg.Set(gate, CVarValue::Bool(true), SetBy::Code, "editor", Permission::Editor) == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Set(cheat, CVarValue::Bool(true), SetBy::Code, "tool", Permission::Player) == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Get(cheat)->AsBool() == true);

    REQUIRE(reg.Set(gate, CVarValue::Bool(false), SetBy::Code, "editor", Permission::Editor) == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Get(cheat)->AsBool() == false);
    REQUIRE(reg.Explain("game.noclip")->setBy == SetBy::Project);
}

TEST_CASE("config apply warns on a cvar category and ignores a document", "[cvar]") {
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(CVarDesc{ "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(false), {}, {}, CVarFlags::Archive, "", "engine" }).IsStale());
    nlohmann::json diagnostics = { {"drawMarkers", true}, {"notACvar", 1} };
    const CVarApplyReport cvars = ApplyCVarCategory(reg, "diagnostics", diagnostics, SetBy::Project, false, "project");
    REQUIRE(cvars.unknownKeys.size() == 1);
    REQUIRE(cvars.unknownKeys[0] == "diagnostics.notACvar");
    reg.Publish();
    REQUIRE(reg.Get(reg.Find("diagnostics.drawMarkers"))->AsBool() == true);

    nlohmann::json input = { {"actionMaps", nlohmann::json::array()} };
    const CVarApplyReport document = ApplyCVarCategory(reg, "input", input, SetBy::Project, true, "project");
    REQUIRE(document.unknownKeys.empty());

    const auto user = std::filesystem::temp_directory_path() / "arcane-cvar-archive-test";
    std::filesystem::remove_all(user);
    WriteCVarArchive(reg, user);
    REQUIRE_FALSE(std::filesystem::exists(user / "diagnostics.json"));

    REQUIRE(reg.Set(reg.Find("diagnostics.drawMarkers"), CVarValue::Bool(false), SetBy::Console) == SetResult::Applied);
    reg.Publish();
    WriteCVarArchive(reg, user);
    REQUIRE(std::filesystem::exists(user / "diagnostics.json"));
    std::filesystem::remove_all(user);
}

TEST_CASE("command line set beats user and loses to code", "[cvar]") {
    CVarRegistry reg;
    const CVarHandle h = reg.Register(CVarDesc{
        "game.speed", CVarType::Int32, CVarValue::Int32(1), {}, {}, CVarFlags::UserSettable, "", "engine" });
    REQUIRE(reg.Set(h, CVarValue::Int32(2), SetBy::User) == SetResult::Applied);
    ApplyCVarCommandLine(reg, { "game.speed=4" }, Permission::Player);
    REQUIRE(reg.Set(h, CVarValue::Int32(9), SetBy::Code) == SetResult::Applied);
    ApplyCVarCommandLine(reg, { "game.speed=5" }, Permission::Player);
    reg.Publish();
    REQUIRE(reg.Get(h)->AsInt32() == 9);
    REQUIRE(reg.Explain("game.speed")->setBy == SetBy::Code);
}

TEST_CASE("a command and a cvar cannot share a name", "[cvar]") {
    CVarRegistry reg;
    REQUIRE(reg.RegisterCommand("game.speed", CVarFlags::None, "", "mod", [](std::string_view, std::string&, void*) {}, nullptr));
    REQUIRE(reg.Register(CVarDesc{ "game.speed", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "", "engine" }).IsStale());
    REQUIRE(reg.LastError().find("command") != std::string::npos);
}

TEST_CASE("console model submits, completes, and refuses a player", "[cvar]") {
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(CVarDesc{
        "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(false), {}, {}, {}, "markers", "engine" }).IsStale());
    REQUIRE_FALSE(reg.Register(CVarDesc{
        "game.speed", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "", "engine" }).IsStale());
    ConsoleModel model;
    model.SetInput("diag");
    const auto matches = model.Complete(reg);
    REQUIRE(matches.size() == 1);
    REQUIRE(matches[0] == "diagnostics.drawMarkers");
    model.SetInput("cvar_explain diagnostics.drawMarkers");
    model.Submit(reg, Permission::Editor);
    REQUIRE(model.Lines().size() == 2);
    REQUIRE(model.Lines().back().text.find("Default") != std::string::npos);
    model.SetInput("game.speed 3");
    model.Submit(reg, Permission::Player);
    REQUIRE_FALSE(model.Lines().back().ok);
    REQUIRE(reg.Get(reg.Find("game.speed"))->AsInt32() == 1);

    // An accepted set is PENDING after Submit: the console is a writer like
    // any other, and only the frame driver's Publish makes it visible
    // (spec 6.4 -- read-your-own-writes is deliberately not provided).
    model.SetInput("game.speed 3");
    model.Submit(reg, Permission::Editor);
    REQUIRE(model.Lines().back().ok);
    REQUIRE(reg.Get(reg.Find("game.speed"))->AsInt32() == 1);
    reg.Publish();
    REQUIRE(reg.Get(reg.Find("game.speed"))->AsInt32() == 3);
}

TEST_CASE("render.meshCull defaults on and publishes off", "[cvar]") {
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle handle = reg.Find("render.meshCull");
    if (handle.IsStale()) return;   // Dist compiles the Dev cvar out; missing means on
    REQUIRE(MeshCullFrustumEnabled());
    REQUIRE(reg.Set(handle, CVarValue::Bool(false), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    REQUIRE_FALSE(MeshCullFrustumEnabled());
    REQUIRE(reg.Set(handle, CVarValue::Bool(true), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    REQUIRE(MeshCullFrustumEnabled());
}

TEST_CASE("Dev cvars are absent when the registry is built without them", "[cvar]") {
    CVarRegistry reg{ false };
    REQUIRE(reg.Register(CVarDesc{ "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(false), {}, {}, CVarFlags::Dev, "", "engine" }).IsStale());
    REQUIRE(reg.Find("diagnostics.drawMarkers").IsStale());
    REQUIRE_FALSE(reg.Register(CVarDesc{ "game.speed", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "", "engine" }).IsStale());
}

namespace
{
    ARC_CVAR_RANGED("tests.rangedProbe", "tests", Int32, CVarValue::Int32(5),
                    CVarValue::Int32(1), CVarValue::Int32(10), CVarFlags::Archive,
                    "ARC_CVAR_RANGED probe (CVarRegistryTest).");
}

TEST_CASE("ARC_CVAR_RANGED registers its range and its module", "[cvar]") {
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("tests.rangedProbe");
    REQUIRE_FALSE(h.IsStale());
    CHECK(reg.Get(h)->AsInt32() == 5);

    reg.Set(h, CVarValue::Int32(50), SetBy::Console);
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 10);            // max applied
    reg.Set(h, CVarValue::Int32(-4), SetBy::Console);
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 1);             // min applied

    const CVarHandle dup = reg.Register(CVarDesc{
        "tests.rangedProbe", CVarType::Int32, CVarValue::Int32(5),
        std::nullopt, std::nullopt, CVarFlags::None, "", "engine" });
    CHECK(dup.IsStale());
    CHECK(reg.LastError().find("module 'tests'") != std::string::npos);   // declared by the macro's module

    reg.Set(h, CVarValue::Int32(5), SetBy::Console);
    reg.Publish();
}
