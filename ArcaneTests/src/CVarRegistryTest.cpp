#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <cstdint>
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

TEST_CASE("Dev cvars are absent when the registry is built without them", "[cvar]") {
    CVarRegistry reg{ false };
    REQUIRE(reg.Register(CVarDesc{ "diagnostics.drawMarkers", CVarType::Bool, CVarValue::Bool(false), {}, {}, CVarFlags::Dev, "", "engine" }).IsStale());
    REQUIRE(reg.Find("diagnostics.drawMarkers").IsStale());
    REQUIRE_FALSE(reg.Register(CVarDesc{ "game.speed", CVarType::Int32, CVarValue::Int32(1), {}, {}, {}, "", "engine" }).IsStale());
}
