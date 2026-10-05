// The EditorUser rung (settings arc S2, spec s3.3, s11.1): SetBy 35 between
// Project and User, ClearRung, the declared metadata on Explain, and the two
// archives -- the machine folder takes only PreferencesMachine values; the
// project's Saved/Config takes the "This project" overrides and forgets one
// when the switch goes back to "All projects".
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>

#include <Json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace Arcane;

namespace
{
    namespace fs = std::filesystem;

    fs::path FreshDir(const char* tag)
    {
        std::error_code ec;
        fs::path d = fs::temp_directory_path() / (std::string("arcane_s2_") + tag);
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        return d;
    }

    nlohmann::json ReadJson(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return nlohmann::json::parse(in, nullptr, false);
    }

    CVarHandle RegisterPref(CVarRegistry& reg, std::string_view name, SettingScope scope, std::int32_t def)
    {
        CVarDesc d;
        d.name = name;
        d.type = CVarType::Int32;
        d.defaultValue = CVarValue::Int32(def);
        d.flags = CVarFlags::Archive;
        d.help = "EditorUser rung probe.";
        d.module = "editoruser-test";
        d.audience = Audience::Editor;
        d.scope = scope;
        d.apply = ApplyMode::Live;
        return reg.Register(d);
    }
}

TEST_CASE("SetBy::EditorUser sits between Project and User; ClearRung peels one rung at a time", "[settings][cvar]")
{
    static_assert(SetBy::Project < SetBy::EditorUser && SetBy::EditorUser < SetBy::User);
    CVarRegistry reg;
    const CVarHandle h = RegisterPref(reg, "eu.value", SettingScope::PreferencesMachine, 1);
    REQUIRE_FALSE(h.IsStale());
    ApplyCVarCategory(reg, "eu", nlohmann::json{ { "value", 2 } }, SetBy::Project, false, "project");
    ApplyCVarCategory(reg, "eu", nlohmann::json{ { "value", 3 } }, SetBy::EditorUser, false, "editor-user");
    ApplyCVarCategory(reg, "eu", nlohmann::json{ { "value", 4 } }, SetBy::User, false, "user");
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 4);
    CHECK(reg.ClearRung(h, SetBy::User));
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 3);
    CHECK(reg.Explain("eu.value")->setBy == SetBy::EditorUser);
    CHECK(reg.ClearRung(h, SetBy::EditorUser));
    CHECK_FALSE(reg.ClearRung(h, SetBy::EditorUser));   // nothing left at that rung
    CHECK_FALSE(reg.ClearRung(h, SetBy::Default));      // the default is never cleared
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 2);
}

TEST_CASE("cvar_explain names the EditorUser rung; Explain reports the declared audience, scope and apply", "[settings][cvar]")
{
    CVarRegistry reg;
    const CVarHandle h = RegisterPref(reg, "eu.named", SettingScope::PreferencesMachine, 1);
    REQUIRE(reg.Set(h, CVarValue::Int32(5), SetBy::EditorUser, "editor-user") == SetResult::Applied);
    reg.Publish();
    const ExecResult r = reg.Execute("cvar_explain eu.named", CVarContext::Editor);
    REQUIRE(r.ok);
    CHECK(r.text.find("EditorUser") != std::string::npos);
    const auto e = reg.Explain("eu.named");
    REQUIRE(e);
    CHECK(e->scope == SettingScope::PreferencesMachine);
    CHECK(e->audience == Audience::Editor);
    CHECK(e->apply == ApplyMode::Live);
}

TEST_CASE("The EditorUser archive holds only machine-wide preferences; the User archive holds and forgets this-project overrides", "[settings][cvar]")
{
    const fs::path machine = FreshDir("eu_machine");
    const fs::path project = FreshDir("eu_project");
    CVarRegistry reg;
    const CVarHandle theme = RegisterPref(reg, "euarch.theme", SettingScope::PreferencesMachine, 1);
    const CVarHandle undo = RegisterPref(reg, "euarch.undo", SettingScope::PreferencesProject, 1);
    const CVarHandle font = RegisterPref(reg, "euarch.font", SettingScope::PreferencesMachine, 1);
    REQUIRE(reg.Set(theme, CVarValue::Int32(2), SetBy::EditorUser, "editor-user") == SetResult::Applied);
    REQUIRE(reg.Set(undo, CVarValue::Int32(3), SetBy::EditorUser, "editor-user") == SetResult::Applied);   // Pref-P on the machine rung: not this archive's
    REQUIRE(reg.Set(font, CVarValue::Int32(4), SetBy::EditorUser, "editor-user") == SetResult::Applied);
    REQUIRE(reg.Set(font, CVarValue::Int32(5), SetBy::User, "user") == SetResult::Applied);                // "This project"
    REQUIRE(reg.Set(undo, CVarValue::Int32(6), SetBy::User, "user") == SetResult::Applied);
    reg.Publish();

    WriteCVarArchive(reg, machine, SetBy::EditorUser);
    WriteCVarArchive(reg, project, SetBy::User);
    const nlohmann::json m = ReadJson(machine / "euarch.json");
    INFO(m.dump());
    CHECK(m.size() == 2);
    CHECK(m.at("theme") == 2);
    CHECK(m.at("font") == 4);
    const nlohmann::json p = ReadJson(project / "euarch.json");
    INFO(p.dump());
    CHECK(p.size() == 2);
    CHECK(p.at("font") == 5);
    CHECK(p.at("undo") == 6);

    // "All projects" again: the User record goes, and so does the key on disk.
    REQUIRE(reg.ClearRung(font, SetBy::User));
    reg.Publish();
    WriteCVarArchive(reg, project, SetBy::User);
    const nlohmann::json after = ReadJson(project / "euarch.json");
    INFO(after.dump());
    CHECK_FALSE(after.contains("font"));
    CHECK(after.at("undo") == 6);

    const fs::path other = FreshDir("eu_other");
    WriteCVarArchive(reg, other, SetBy::Project);                 // not an archive rung: writes nothing
    CHECK_FALSE(fs::exists(other / "euarch.json"));

    std::error_code ec;
    fs::remove_all(machine, ec);
    fs::remove_all(project, ec);
    fs::remove_all(other, ec);
}

TEST_CASE("ApplyCVarDirectory applies a machine folder at the EditorUser rung", "[settings][cvar]")
{
    const fs::path machine = FreshDir("eu_apply");
    {
        std::ofstream(machine / "euapply.json", std::ios::binary) << R"({ "x": 7 })";
    }
    CVarRegistry reg;
    const CVarHandle h = RegisterPref(reg, "euapply.x", SettingScope::PreferencesMachine, 1);
    const CVarApplyReport report = ApplyCVarDirectory(reg, machine, SetBy::EditorUser, "editor-user");
    CHECK(report.unknownKeys.empty());
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 7);
    CHECK(reg.Explain("euapply.x")->setBy == SetBy::EditorUser);
    std::error_code ec;
    fs::remove_all(machine, ec);
}

TEST_CASE("ApplyLayersFor re-applies the EditorUser rung to a reloaded module's cvars", "[settings][cvar]")
{
    const fs::path machine = FreshDir("eu_layers");
    {
        std::ofstream(machine / "eulayer.json", std::ios::binary) << R"({ "x": 4 })";
    }
    CVarRegistry reg;
    CVarDesc d;
    d.name = "eulayer.x";
    d.type = CVarType::Int32;
    d.defaultValue = CVarValue::Int32(1);
    d.flags = CVarFlags::Archive;
    d.help = "Layer probe.";
    d.module = "eulayer-module";
    d.audience = Audience::Editor;
    d.scope = SettingScope::PreferencesMachine;
    const CVarHandle h = reg.Register(d);
    REQUIRE_FALSE(h.IsStale());
    LayerSources layers;
    layers.dirs.push_back(CVarLayerDir{ SetBy::EditorUser, machine, "editor-user" });
    reg.ApplyLayersFor("eulayer-module", layers);
    CHECK(reg.Get(h)->AsInt32() == 4);
    CHECK(reg.Explain("eulayer.x")->setBy == SetBy::EditorUser);
    std::error_code ec;
    fs::remove_all(machine, ec);
}
