// Settings arc S7 (spec s8.1): the PlayerSafe slice of the registry, read and
// written in the session's context. This file starts with the descriptor read
// (CVarRegistry::ListEx) that PlayerSettings::List is built on.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/PlayerSettings.hpp>
#include <Arcane/Plugin/PluginHost.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace Arcane;

namespace
{
    const CVarListEntryEx* FindEntry(const std::vector<CVarListEntryEx>& list, std::string_view name)
    {
        const auto it = std::find_if(list.begin(), list.end(), [&](const CVarListEntryEx& e) { return e.name == name; });
        return it == list.end() ? nullptr : &*it;
    }
}

TEST_CASE("CVarRegistry::ListEx carries every descriptor field a settings menu needs", "[player-settings]")
{
    CVarRegistry reg;

    CVarDesc vol;
    vol.name = "audio.masterVolume";
    vol.type = CVarType::Float32;
    vol.defaultValue = CVarValue::Float32(0.8f);
    vol.min = CVarValue::Float32(0.0f);
    vol.max = CVarValue::Float32(1.0f);
    vol.flags = CVarFlags::Archive;
    vol.help = "Master output volume.";
    vol.module = "test-s7";
    vol.audience = Audience::PlayerSafe;
    vol.scope = SettingScope::Project;
    vol.apply = ApplyMode::Live;
    vol.order = 3;
    REQUIRE_FALSE(reg.Register(vol).IsStale());

    CVarDesc quality;
    quality.name = "render.quality";
    quality.type = CVarType::Enum;
    quality.defaultValue = CVarValue::Enum(1);
    quality.enumNames = { "Low", "Medium", "High" };
    quality.help = "Overall quality preset.";
    quality.module = "test-s7";
    quality.audience = Audience::PlayerSafe;
    quality.apply = ApplyMode::Restart;
    quality.displayName = "Quality Preset";
    quality.categoryPath = "Render";
    REQUIRE_FALSE(reg.Register(quality).IsStale());

    CVarDesc hidden;
    hidden.name = "audio.secretKnob";
    hidden.type = CVarType::Bool;
    hidden.defaultValue = CVarValue::Bool(false);
    hidden.flags = CVarFlags::Hidden;
    hidden.module = "test-s7";
    hidden.audience = Audience::PlayerSafe;
    REQUIRE_FALSE(reg.Register(hidden).IsStale());

    REQUIRE(reg.Set(reg.Find("audio.masterVolume"), CVarValue::Float32(0.5f), SetBy::User, "user",
                    CVarContext::Editor) == SetResult::Applied);
    reg.PublishImmediate();

    const std::vector<CVarListEntryEx> list = reg.ListEx();
    const CVarListEntryEx* v = FindEntry(list, "audio.masterVolume");
    REQUIRE(v);
    CHECK(v->displayName == "Master Volume");      // derived from the last segment
    CHECK(v->categoryPath == "Audio");             // registered derivation from the dotted name
    const auto volumeMetadata = reg.Metadata(reg.Find("audio.masterVolume"));
    REQUIRE(volumeMetadata);
    CHECK(v->categoryPath == volumeMetadata->categoryPath);
    CHECK(v->help == "Master output volume.");
    CHECK(v->type == CVarType::Float32);
    CHECK(v->audience == Audience::PlayerSafe);
    CHECK(v->scope == SettingScope::Project);
    CHECK(v->apply == ApplyMode::Live);
    CHECK(v->order == 3);
    REQUIRE(v->min);
    CHECK(v->min->AsFloat32() == 0.0f);
    REQUIRE(v->max);
    CHECK(v->max->AsFloat32() == 1.0f);
    CHECK(v->value.AsFloat32() == 0.5f);           // published
    CHECK(v->defaultValue.AsFloat32() == 0.8f);    // the Default rung, not the winner

    const CVarListEntryEx* q = FindEntry(list, "render.quality");
    REQUIRE(q);
    CHECK(q->displayName == "Quality Preset");     // declared names win over derivation
    CHECK(q->categoryPath == "Render");            // explicit display path keeps its case
    const auto qualityMetadata = reg.Metadata(reg.Find("render.quality"));
    REQUIRE(qualityMetadata);
    CHECK(q->categoryPath == qualityMetadata->categoryPath);
    CHECK(q->type == CVarType::Enum);
    CHECK(q->enumNames == std::vector<std::string>{ "Low", "Medium", "High" });
    CHECK(q->value.AsEnum() == 1);
    CHECK(q->apply == ApplyMode::Restart);

    CHECK(FindEntry(list, "audio.secretKnob") == nullptr);   // Hidden is never listed
}

namespace
{
    void RegisterKnob(CVarRegistry& reg, std::string_view name, CVarValue def, Audience audience,
                      CVarFlags flags = CVarFlags::None)
    {
        CVarDesc d;
        d.name = name;
        d.type = def.type;
        d.defaultValue = def;
        d.flags = flags;
        d.help = "S7 test knob.";
        d.module = "test-s7";
        d.audience = audience;
        const CVarHandle h = reg.Register(d);
        INFO(reg.LastError());
        REQUIRE_FALSE(h.IsStale());
    }
}

TEST_CASE("CVarContextFor: single-player and a listen host are LocalHost, a connected player is Client, a dedicated server is ServerAdmin", "[player-settings]")
{
    CHECK(CVarContextFor(NetMode::Standalone) == CVarContext::LocalHost);
    CHECK(CVarContextFor(NetMode::ListenServer) == CVarContext::LocalHost);
    CHECK(CVarContextFor(NetMode::Client) == CVarContext::Client);
    CHECK(CVarContextFor(NetMode::DedicatedServer) == CVarContext::ServerAdmin);
}

TEST_CASE("PlayerSettings::List returns PlayerSafe settings only, by category prefix, menu-ordered", "[player-settings]")
{
    CVarRegistry reg;
    RegisterKnob(reg, "audio.masterVolume", CVarValue::Float32(0.8f), Audience::PlayerSafe);
    RegisterKnob(reg, "audio.channels", CVarValue::Int32(2), Audience::PlayerSafe);
    RegisterKnob(reg, "audio.mixerBlockSize", CVarValue::Int32(512), Audience::Game);       // not player-safe
    RegisterKnob(reg, "audiox.knob", CVarValue::Bool(true), Audience::PlayerSafe);           // prefix boundary
    RegisterKnob(reg, "render.vsync", CVarValue::Bool(true), Audience::PlayerSafe);

    const auto audio = PlayerSettings::List(reg, "audio");
    REQUIRE(audio.size() == 2);
    CHECK(audio[0].name == "audio.channels");          // same category, same order -> by name
    CHECK(audio[1].name == "audio.masterVolume");
    for (const CVarListEntryEx& e : audio) CHECK(e.audience == Audience::PlayerSafe);

    CHECK(PlayerSettings::List(reg, "audio.").size() == 2);
    CHECK(PlayerSettings::List(reg, "").size() == 4);  // every PlayerSafe row, no Game row
}

TEST_CASE("PlayerSettings::Set follows the audience x context table: LocalHost reaches Server settings, Client reaches only player-safe ones", "[player-settings]")
{
    CVarRegistry reg;
    RegisterKnob(reg, "audio.masterVolume", CVarValue::Float32(0.8f), Audience::PlayerSafe, CVarFlags::Archive);
    RegisterKnob(reg, "test.s7.gameKnob", CVarValue::Float32(1.0f), Audience::Game);
    RegisterKnob(reg, "test.s7.serverKnob", CVarValue::Int32(60), Audience::Server);
    RegisterKnob(reg, "test.s7.cheatServer", CVarValue::Bool(false), Audience::Server, CVarFlags::Cheat);

    using PlayerSettings::Set;
    CHECK(Set(reg, "audio.masterVolume", CVarValue::Float32(0.25f), CVarContext::LocalHost) == SetResult::Applied);
    CHECK(Set(reg, "audio.masterVolume", CVarValue::Float32(0.30f), CVarContext::Client) == SetResult::Applied);
    const auto explained = reg.Explain("audio.masterVolume");
    REQUIRE(explained);
    CHECK(explained->setBy == SetBy::User);              // a player's choice is the User rung

    CHECK(Set(reg, "test.s7.gameKnob", CVarValue::Float32(2.0f), CVarContext::LocalHost) == SetResult::Denied);
    CHECK(Set(reg, "test.s7.gameKnob", CVarValue::Float32(2.0f), CVarContext::Client) == SetResult::Denied);
    CHECK(Set(reg, "test.s7.serverKnob", CVarValue::Int32(30), CVarContext::LocalHost) == SetResult::Applied);
    CHECK(Set(reg, "test.s7.serverKnob", CVarValue::Int32(20), CVarContext::Client) == SetResult::Denied);
    CHECK(Set(reg, "test.s7.cheatServer", CVarValue::Bool(true), CVarContext::LocalHost) == SetResult::Denied);   // cheats off

    // Not a player context: refused before the registry is asked.
    CHECK(Set(reg, "audio.masterVolume", CVarValue::Float32(0.5f), CVarContext::ServerAdmin) == SetResult::Denied);
    CHECK(Set(reg, "audio.masterVolume", CVarValue::Float32(0.5f), CVarContext::Editor) == SetResult::Denied);

    CHECK(Set(reg, "no.such.cvar", CVarValue::Bool(true), CVarContext::LocalHost) == SetResult::Stale);
    CHECK(Set(reg, "audio.masterVolume", CVarValue::Int32(1), CVarContext::LocalHost) == SetResult::TypeMismatch);

    reg.PublishImmediate();
    CHECK(reg.Get(reg.Find("audio.masterVolume"))->AsFloat32() == 0.30f);
    CHECK(reg.Get(reg.Find("test.s7.serverKnob"))->AsInt32() == 30);
}

TEST_CASE("PlayerSettings follows the primary world's net mode through the plugin host", "[player-settings]")
{
    struct ResetSession { ~ResetSession() { PlayerSettings::SetSessionMode(NetMode::Standalone); } } reset;

    Runtime runtime(Test::Process(), NetMode::Client);
    PluginHost host(Test::Process(), std::filesystem::path("ReferenceGameUnderTest.dll"));
    REQUIRE(host.AttachRuntime(runtime));
    REQUIRE(host.Load());                                   // no ClientRuntime: ReferenceGame's OnInit returns true
    CHECK(PlayerSettings::SessionMode() == NetMode::Client);
    CHECK(PlayerSettings::SessionContext() == CVarContext::Client);

    runtime.SetNetMode(NetMode::ListenServer);
    host.RefreshEngineContext();
    CHECK(PlayerSettings::SessionContext() == CVarContext::LocalHost);
    host.Unload();
}

TEST_CASE("the inventory's PlayerSafe rows are what a game's settings menu lists; no editor setting ever is", "[player-settings]")
{
    // Names from the frozen inventory (S5). If S5's review renamed a row, use the frozen name.
    // S6 owns converting those rows onto the process registry. This S7 lane is
    // cut from S1+S5 (DAG: S7-1..S7-8 need only S1), so the names are absent
    // until S6 lands. When a name is registered, List("") must include it.
    const std::vector<CVarListEntryEx> all = PlayerSettings::List("");
    for (const char* name : { "render.backend", "render.vsync", "render.window.width", "render.window.height",
                              "render.adapter", "render.allowTearing", "render.textureAnisotropy",
                              "input.deadzone.defaultMin", "input.deadzone.defaultMax", "audio.channels" })
    {
        INFO(name);
        if (CVarRegistry::Get().Find(name).IsStale())
        {
            WARN("inventory PlayerSafe row not registered yet (S6): " << name);
            continue;
        }
        CHECK(std::any_of(all.begin(), all.end(), [&](const CVarListEntryEx& e) { return e.name == name; }));
    }
    for (const CVarListEntryEx& e : all)
    {
        INFO(e.name);
        CHECK(e.audience == Audience::PlayerSafe);
        CHECK_FALSE(e.name.starts_with("editor."));
    }
}
