// Renames (settings spec 2026-10-03 s4.7, O7): an old name resolves in config
// files, --set and the console with ONE warning per old name, and the next
// archive write migrates the user's file to the new name.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Base/Log.hpp>

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace Arcane;

namespace
{
    ARC_CVAR(cvar_aliasRenamed, "tests.alias.renamed", std::int32_t, 4, .help = "ARC_CVAR_ALIAS probe target.");
    ARC_CVAR_ALIAS("tests.alias.original", "tests.alias.renamed");

    struct LogCapture
    {
        std::string last;
        int count = 0;
        std::shared_ptr<spdlog::sinks::callback_sink_mt> sink;
        LogCapture()
        {
            sink = std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg& m) {
                last.assign(m.payload.data(), m.payload.size());
                ++count;
            });
            Log::Engine()->sinks().push_back(sink);
        }
        ~LogCapture()
        {
            auto& sinks = Log::Engine()->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
        }
        LogCapture(const LogCapture&) = delete;
        LogCapture& operator=(const LogCapture&) = delete;
    };

    void WithRenamed(CVarRegistry& reg)
    {
        REQUIRE_FALSE(reg.Register(CVarDesc{ .name = "render.newName", .type = CVarType::Int32, .defaultValue = CVarValue::Int32(1),
                                             .flags = CVarFlags::Archive, .help = "renamed probe", .module = "test" }).IsStale());
        REQUIRE(reg.RegisterAlias("render.oldName", "render.newName"));
    }

    std::string ReadText(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
}

TEST_CASE("an alias resolves for the console, warning once, while Find stays exact", "[cvar]")
{
    CVarRegistry reg;
    WithRenamed(reg);
    CHECK(reg.Find("render.oldName").IsStale());
    CHECK(reg.AliasTarget("render.oldName") == "render.newName");
    CHECK(reg.AliasTarget("render.newName").empty());

    LogCapture log;
    CHECK(reg.Resolve("render.oldName") == reg.Find("render.newName"));
    const ExecResult set = reg.Execute("render.oldName 7", CVarContext::Editor);
    CHECK(set.ok);
    CHECK(set.text == "render.newName set (pending publish)");
    reg.Publish();
    CHECK(reg.Get(reg.Find("render.newName"))->AsInt32() == 7);
    CHECK(reg.Execute("render.oldName", CVarContext::Editor).text == "render.newName = 7");
    CHECK(reg.Explain("render.oldName")->name == "render.newName");
    CHECK(log.count == 1);
    CHECK(log.last.find("render.oldName") != std::string::npos);
    CHECK(log.last.find("render.newName") != std::string::npos);
}

TEST_CASE("an alias resolves for --set", "[cvar]")
{
    CVarRegistry cli;
    WithRenamed(cli);
    LogCapture log;
    ApplyCVarCommandLine(cli, { "render.oldName=9" }, CVarContext::Editor);
    cli.Publish();
    CHECK(cli.Get(cli.Find("render.newName"))->AsInt32() == 9);
    CHECK(cli.Explain("render.newName")->setBy == SetBy::CommandLine);
    CHECK(log.count == 1);                         // the rename warning; the set itself was applied
}

TEST_CASE("an alias resolves in a config file, and the next archive write keeps only the new name", "[cvar]")
{
    const auto user = std::filesystem::temp_directory_path() / "arcane-cvar-alias-archive";
    std::filesystem::remove_all(user);
    std::filesystem::create_directories(user);
    {
        std::ofstream out(user / "render.json", std::ios::binary);
        out << R"({"oldName": 3, "foreign": 1})";
    }
    CVarRegistry reg;
    WithRenamed(reg);
    const CVarApplyReport report = ApplyCVarDirectory(reg, user, SetBy::User, "user");
    CHECK(report.unknownKeys == std::vector<std::string>{ "render.foreign" });   // the old name resolved; only the foreign key is unknown
    reg.Publish();
    CHECK(reg.Get(reg.Find("render.newName"))->AsInt32() == 3);

    WriteCVarArchive(reg, user);
    const auto doc = nlohmann::json::parse(ReadText(user / "render.json"));
    INFO(doc.dump());
    CHECK(doc.at("newName") == 3);
    CHECK_FALSE(doc.contains("oldName"));
    CHECK(doc.at("foreign") == 1);
    std::filesystem::remove_all(user);
}

TEST_CASE("RegisterAlias refuses names it cannot own and re-points a renamed rename", "[cvar]")
{
    CVarRegistry reg;
    WithRenamed(reg);
    CHECK_FALSE(reg.RegisterAlias("", "render.newName"));
    CHECK_FALSE(reg.RegisterAlias("render.x", "render.x"));
    CHECK_FALSE(reg.RegisterAlias("render.newName", "render.other"));       // a live cvar keeps its name
    CHECK_FALSE(reg.RegisterAlias("cvarlist", "render.newName"));            // so does a command
    CHECK(reg.RegisterAlias("render.oldName", "render.newName"));             // the same pair again: a reloaded module
    CHECK_FALSE(reg.RegisterAlias("render.oldName", "render.elsewhere"));     // one old name, one target
    CHECK_FALSE(reg.RegisterAlias("render.loop", "render.oldName"));          // the target is itself an old name

    CHECK(reg.Register(CVarDesc{ .name = "render.oldName", .type = CVarType::Int32, .defaultValue = CVarValue::Int32(1),
                                 .help = "taken", .module = "test" }).IsStale());
    CHECK(reg.LastError().find("alias") != std::string::npos);
    CHECK_FALSE(reg.RegisterCommand("render.oldName", CVarFlags::None, "taken", "test",
                                    [](std::string_view, std::string&, void*) {}, nullptr));

    REQUIRE(reg.RegisterAlias("a.v1", "a.v2"));
    REQUIRE(reg.RegisterAlias("a.v2", "a.v3"));                               // v2 renamed again: v1 follows
    CHECK(reg.AliasTarget("a.v1") == "a.v3");
    CHECK(reg.AliasTarget("a.v2") == "a.v3");
    CHECK(reg.Aliases().size() == 3);
    CHECK(reg.Aliases().front().first == "a.v1");                             // sorted by old name
}

TEST_CASE("ARC_CVAR_ALIAS registers on the process registry", "[cvar]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    CHECK(reg.AliasTarget("tests.alias.original") == "tests.alias.renamed");
    CHECK(reg.Resolve("tests.alias.original") == cvar_aliasRenamed.Handle());
}
