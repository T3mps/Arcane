// Settings arc S3-1: the registry surface the settings windows read and write
// per row -- describe, enumerate, read one rung, set/revert one rung in rung
// order -- and the revision they rebuild on.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsFixtures.hpp"
#include "Helpers/TestEnvironment.hpp"
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace Arcane;
using Arcane::Test::AddSetting;

TEST_CASE("CVarRegistry::Describe returns every descriptor field the windows draw", "[cvar]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "editor.graph.fitMinZoom", { .type = CVarType::Float32, .def = CVarValue::Float32(0.5f),
        .scope = SettingScope::PreferencesMachine, .apply = ApplyMode::Live, .module = "editor",
        .displayName = "Fit min zoom", .keywords = "zoom frame", .widget = "slider", .categoryPath = "Editor/Graph",
        .min = CVarValue::Float32(0.1f), .max = CVarValue::Float32(2.0f), .audience = Audience::Editor, .order = 3,
        .help = "Smallest zoom a frame-to-fit may pick." }).IsStale());
    const std::optional<CVarDescInfo> d = reg.Describe("editor.graph.fitMinZoom");
    REQUIRE(d.has_value());
    CHECK(d->name == "editor.graph.fitMinZoom");
    CHECK(d->help == "Smallest zoom a frame-to-fit may pick.");
    CHECK(d->displayName == "Fit min zoom");
    CHECK(d->keywords == "zoom frame");
    CHECK(d->widget == "slider");
    CHECK(d->categoryPath == "Editor/Graph");
    CHECK(d->module == "editor");
    CHECK(d->type == CVarType::Float32);
    CHECK(d->defaultValue == CVarValue::Float32(0.5f));
    CHECK(d->min == std::optional<CVarValue>(CVarValue::Float32(0.1f)));
    CHECK(d->max == std::optional<CVarValue>(CVarValue::Float32(2.0f)));
    CHECK(d->audience == Audience::Editor);
    CHECK(d->scope == SettingScope::PreferencesMachine);
    CHECK(d->apply == ApplyMode::Live);
    CHECK(d->order == 3);
    CHECK_FALSE(reg.Describe("no.such.cvar").has_value());
}

TEST_CASE("CVarRegistry::Names is sorted and lists Hidden cvars only on request", "[cvar]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "zeta.a", { .type = CVarType::Bool, .def = CVarValue::Bool(false) }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "alpha.b", { .type = CVarType::Bool, .def = CVarValue::Bool(false), .flags = CVarFlags::Hidden }).IsStale());
    const std::vector<std::string> shown = reg.Names(false);
    const std::vector<std::string> all = reg.Names(true);
    CHECK(std::is_sorted(all.begin(), all.end()));
    CHECK(std::find(shown.begin(), shown.end(), "alpha.b") == shown.end());
    CHECK(std::find(all.begin(), all.end(), "alpha.b") != all.end());
    CHECK(std::find(shown.begin(), shown.end(), "zeta.a") != shown.end());
}

TEST_CASE("CVarRegistry::SetRung writes UNDER a stronger rung; RevertRung uncovers it", "[cvar]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    REQUIRE(reg.Set(reg.Find("render.vsync"), CVarValue::Bool(false), SetBy::User, "user") == SetResult::Applied);
    REQUIRE(reg.SetRung("render.vsync", SetBy::Project, CVarValue::Bool(true), "project"));
    reg.Publish();
    std::optional<CVarExplain> e = reg.Explain("render.vsync");
    REQUIRE(e.has_value());
    CHECK(e->setBy == SetBy::User);                       // the User record still wins
    CHECK(e->pending == CVarValue::Bool(false));
    CHECK(reg.RungValue("render.vsync", SetBy::Project) == std::optional<CVarValue>(CVarValue::Bool(true)));
    REQUIRE(reg.RevertRung("render.vsync", SetBy::User));
    reg.Publish();
    e = reg.Explain("render.vsync");
    CHECK(e->setBy == SetBy::Project);
    CHECK(e->pending == CVarValue::Bool(true));
    CHECK_FALSE(reg.RevertRung("render.vsync", SetBy::User));                                       // nothing left there
    CHECK_FALSE(reg.RevertRung("render.vsync", SetBy::Default));                                    // the default is not a rung
    CHECK_FALSE(reg.SetRung("render.vsync", SetBy::Default, CVarValue::Bool(false), "x"));
    CHECK_FALSE(reg.SetRung("render.vsync", SetBy::Project, CVarValue::Int32(1), "project"));       // type mismatch
    CHECK_FALSE(reg.SetRung("no.such", SetBy::Project, CVarValue::Bool(true), "project"));
}

TEST_CASE("CVarRegistry::SetRung replaces the rung's record and clamps to the declared range", "[cvar]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "editor.undo.maxSteps", { .type = CVarType::Int32, .def = CVarValue::Int32(100),
        .min = CVarValue::Int32(1), .max = CVarValue::Int32(10000) }).IsStale());
    REQUIRE(reg.SetRung("editor.undo.maxSteps", SetBy::EditorUser, CVarValue::Int32(50), "editor-user"));
    REQUIRE(reg.SetRung("editor.undo.maxSteps", SetBy::EditorUser, CVarValue::Int32(99999), "editor-user"));
    CHECK(reg.RungValue("editor.undo.maxSteps", SetBy::EditorUser) == std::optional<CVarValue>(CVarValue::Int32(10000)));
    const std::optional<CVarExplain> e = reg.Explain("editor.undo.maxSteps");
    REQUIRE(e.has_value());
    CHECK(std::count_if(e->history.begin(), e->history.end(),
                        [](const CVarHistoryRecord& h) { return h.by == SetBy::EditorUser; }) == 1);
}

TEST_CASE("CVarRegistry::SetRung refuses an Enum ordinal outside enumNames and leaves history", "[cvar]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "look.mode", { .type = CVarType::Enum, .def = CVarValue::Enum(0),
        .enumNames = { "Off", "Low", "High" } }).IsStale());
    REQUIRE(reg.SetRung("look.mode", SetBy::Project, CVarValue::Enum(1), "project"));
    const std::optional<CVarExplain> before = reg.Explain("look.mode");
    REQUIRE(before.has_value());
    const std::vector<CVarHistoryRecord> history = before->history;

    CHECK_FALSE(reg.SetRung("look.mode", SetBy::Project, CVarValue::Enum(3), "project"));
    CHECK_FALSE(reg.SetRung("look.mode", SetBy::User, CVarValue::Enum(-1), "user"));

    const std::optional<CVarExplain> after = reg.Explain("look.mode");
    REQUIRE(after.has_value());
    REQUIRE(after->history.size() == history.size());
    for (std::size_t i = 0; i < history.size(); ++i)
    {
        CHECK(after->history[i].by == history[i].by);
        CHECK(after->history[i].value == history[i].value);
        CHECK(after->history[i].module == history[i].module);
    }
    CHECK(reg.RungValue("look.mode", SetBy::Project) == std::optional<CVarValue>(CVarValue::Enum(1)));
    CHECK_FALSE(reg.RungValue("look.mode", SetBy::User).has_value());
}

TEST_CASE("CVarRegistry::Revision moves on Register and UnregisterModule, not on Set or Publish", "[cvar]")
{
    CVarRegistry reg;
    const std::uint64_t r0 = reg.Revision();
    REQUIRE_FALSE(AddSetting(reg, "game.speed", { .type = CVarType::Float32, .def = CVarValue::Float32(1.0f), .module = "TestGame" }).IsStale());
    const std::uint64_t r1 = reg.Revision();
    CHECK(r1 != r0);
    REQUIRE(reg.SetRung("game.speed", SetBy::Project, CVarValue::Float32(2.0f), "project"));
    reg.Publish();
    CHECK(reg.Revision() == r1);
    reg.UnregisterModule("TestGame");
    CHECK(reg.Revision() != r1);
}

namespace
{
    std::string ReadFileText(const std::filesystem::path& f)
    {
        std::ifstream in(f, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    void WriteFileText(const std::filesystem::path& f, const std::string& text)
    {
        std::filesystem::create_directories(f.parent_path());
        std::ofstream(f, std::ios::binary) << text;
    }
}

TEST_CASE("WriteCVarRungArchive updates only the named keys: written, removed, foreign and unnamed keys kept", "[cvar]")
{
    Arcane::Test::TempDir dir("rung-archive");
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "render.a", { .type = CVarType::Int32, .def = CVarValue::Int32(1) }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "render.b", { .type = CVarType::Bool, .def = CVarValue::Bool(false) }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "render.untouched", { .type = CVarType::Int32, .def = CVarValue::Int32(0) }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "render.cheat", { .type = CVarType::Bool, .def = CVarValue::Bool(false), .flags = CVarFlags::Cheat }).IsStale());
    WriteFileText(dir.path / "render.json", R"({"a": 1, "b": true, "foreign": 7, "untouched": 3})");
    REQUIRE(reg.SetRung("render.a", SetBy::Project, CVarValue::Int32(5), "project"));
    REQUIRE(reg.SetRung("render.cheat", SetBy::Project, CVarValue::Bool(true), "project"));
    const std::vector<std::string> names{ "render.a", "render.b", "render.cheat", "physics.gone" };
    WriteCVarRungArchive(reg, SetBy::Project, dir.path, names);

    const auto doc = nlohmann::json::parse(ReadFileText(dir.path / "render.json"));
    INFO(doc.dump());
    CHECK(doc.at("a") == 5);                    // written
    CHECK_FALSE(doc.contains("b"));             // named, and the rung holds nothing: removed
    CHECK(doc.at("foreign") == 7);              // not a registered cvar: kept
    CHECK(doc.at("untouched") == 3);            // not named: kept (another editor may own it)
    CHECK_FALSE(doc.contains("cheat"));         // Cheat is a session value, never persisted
    CHECK_FALSE(std::filesystem::exists(dir.path / "physics.json"));   // an unregistered name touches nothing
    CHECK_FALSE(std::filesystem::exists(dir.path / "render.json.tmp"));
}

TEST_CASE("WriteCVarRungArchive creates a category, updates a nested leaf, and deletes a file it empties", "[cvar]")
{
    Arcane::Test::TempDir dir("rung-archive-shapes");
    CVarRegistry reg;
    REQUIRE_FALSE(AddSetting(reg, "editor.graph.zoom", { .type = CVarType::Float32, .def = CVarValue::Float32(1.0f) }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "editor.legend", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    REQUIRE_FALSE(AddSetting(reg, "solo.k", { .type = CVarType::Int32, .def = CVarValue::Int32(0) }).IsStale());
    WriteFileText(dir.path / "editor.json", R"({"graph": {"zoom": 2.0}})");
    WriteFileText(dir.path / "solo.json", R"({"k": 4})");
    REQUIRE(reg.SetRung("editor.graph.zoom", SetBy::EditorUser, CVarValue::Float32(3.0f), "editor-user"));
    REQUIRE(reg.SetRung("editor.legend", SetBy::EditorUser, CVarValue::Bool(false), "editor-user"));
    const std::vector<std::string> names{ "editor.graph.zoom", "editor.legend", "solo.k" };
    WriteCVarRungArchive(reg, SetBy::EditorUser, dir.path, names);

    const auto doc = nlohmann::json::parse(ReadFileText(dir.path / "editor.json"));
    INFO(doc.dump());
    CHECK(doc.at("graph").at("zoom") == 3.0f);  // the nested leaf, updated in place
    CHECK(doc.at("legend") == false);
    CHECK_FALSE(std::filesystem::exists(dir.path / "solo.json"));   // its only key removed: the file goes
}
