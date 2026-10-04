// Settings spec s4.4 (O1/O2): a module's cvars, commands and callbacks live
// exactly as long as its image, and the config rungs reach them when it
// (re)loads. Uses the HotReloadPlugin fixtures beside ArcaneTests.exe -- run
// FROM the exe dir. [cvar][hotreload]

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Project/Project.hpp>

#include "Helpers/CVarTestDesc.hpp"
#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    void RegisterFixtureTypes(Arcane::Runtime& rt)
    {
        rt.Components()->RegisterComponent<Arcane::HotReloadTest::Pulse>();
        rt.Components()->RegisterComponent<Arcane::HotReloadTest::RoleCounters>();
    }

    void RestoreV1()
    {
        fs::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll",
                      fs::copy_options::overwrite_existing);
    }

    // Log::Engine() is the plugin's logger too (PluginHostTest's capture shape).
    struct LogCapture
    {
        std::string text;
        std::shared_ptr<spdlog::sinks::callback_sink_mt> sink;
        LogCapture()
        {
            sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
                [this](const spdlog::details::log_msg& m) { text.append(m.payload.data(), m.payload.size()).push_back('\n'); });
            Arcane::Log::Engine()->sinks().push_back(sink);
        }
        ~LogCapture()
        {
            auto& sinks = Arcane::Log::Engine()->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
        }
    };

    // Non-overlapping occurrences of `needle` in `text`.
    std::size_t CountOf(const std::string& text, std::string_view needle)
    {
        std::size_t count = 0;
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
            ++count;
        return count;
    }

    // Changes console.historySize, so every callback on it fires.
    void BumpHistorySize(Arcane::CVarRegistry& reg)
    {
        const Arcane::CVarHandle h = reg.Find("console.historySize");
        const std::int32_t now = reg.Get(h)->AsInt32();
        REQUIRE(reg.Set(h, Arcane::CVarValue::Int32(now + 1), Arcane::SetBy::Code, "lifetime-test") == Arcane::SetResult::Applied);
        reg.Publish();
    }

    fs::path ScratchProject(const char* tag)
    {
        const fs::path dir = fs::temp_directory_path() / (std::string("arcane_s1_lifetime_") + tag);
        std::error_code ec;
        fs::remove_all(dir, ec);
        REQUIRE(Arcane::Project::Create(dir / "P", "LifetimeProbe").has_value());
        return dir;
    }
}

TEST_CASE("a module's cvars, commands and callbacks leave with its image", "[cvar][hotreload]")
{
    RestoreV1();
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    Arcane::Runtime rt(Arcane::Test::Process());
    RegisterFixtureTypes(rt);
    Arcane::PluginHost host(Arcane::Test::Process(), fs::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());

    const Arcane::CVarHandle step = reg.Find("hotreload.step");
    REQUIRE_FALSE(step.IsStale());
    CHECK(reg.ModuleOf(step) == "HotReloadPluginV1");     // the SOURCE stem, not the HotReloadPluginV1_1.dll copy
    CHECK(reg.Get(step)->AsInt32() == 1);
    const Arcane::ExecResult ping = reg.Execute("hotreload.ping", Arcane::CVarContext::Editor);
    CHECK(ping.ok);
    CHECK(ping.text == "step 1");
    {
        LogCapture log;
        BumpHistorySize(reg);
        CHECK(log.text.find("HotReloadPlugin: console.historySize changed (step 1)") != std::string::npos);
    }

    host.Unload();
    // FIRST, as a REQUIRE: before the fix, every line after it calls into the unmapped image.
    REQUIRE(reg.Find("hotreload.step").IsStale());
    CHECK_FALSE(reg.Get(step).has_value());
    CHECK_FALSE(reg.Execute("hotreload.ping", Arcane::CVarContext::Editor).ok);
    {
        LogCapture log;
        BumpHistorySize(reg);                              // the module's callback must not run
        CHECK(log.text.find("HotReloadPlugin:") == std::string::npos);
    }
    reg.UnregisterModule("lifetime-test");
    reg.Publish();
}

TEST_CASE("a hot reload re-registers from the NEW image; an ABI-refused image leaves nothing behind", "[cvar][hotreload]")
{
    RestoreV1();
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    Arcane::Runtime rt(Arcane::Test::Process());
    RegisterFixtureTypes(rt);
    Arcane::PluginHost host(Arcane::Test::Process(), fs::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    REQUIRE(reg.Execute("hotreload.ping", Arcane::CVarContext::Editor).text == "step 1");

    fs::copy_file("HotReloadPluginV2.dll", "HotReloadPluginV1.dll", fs::copy_options::overwrite_existing);
    REQUIRE(host.ForceReload());
    REQUIRE_FALSE(reg.Find("hotreload.step").IsStale());
    CHECK(reg.Get(reg.Find("hotreload.step"))->AsInt32() == 10);
    CHECK(reg.Execute("hotreload.ping", Arcane::CVarContext::Editor).text == "step 10");

    // Bad's statics run inside LoadLibrary, THEN the ABI gate refuses it.
    fs::copy_file("HotReloadPluginBad.dll", "HotReloadPluginV1.dll", fs::copy_options::overwrite_existing);
    CHECK_FALSE(host.ForceReload());                       // rolled back to the last-good (V2) image
    CHECK(reg.Execute("hotreload.ping", Arcane::CVarContext::Editor).text == "step 10");
    CHECK(reg.Get(reg.Find("hotreload.step"))->AsInt32() == 10);

    host.Unload();
    CHECK(reg.Find("hotreload.step").IsStale());
    RestoreV1();
}

TEST_CASE("an unload flushes the module's unsaved User values to the archive first", "[cvar][hotreload]")
{
    RestoreV1();
    const fs::path dir = ScratchProject("flush");
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    Arcane::Runtime rt(Arcane::Test::Process());
    RegisterFixtureTypes(rt);
    rt.SetUserCVarArchiving(true);
    REQUIRE(rt.OpenProject(dir / "P"));
    Arcane::PluginHost host(Arcane::Test::Process(), fs::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    REQUIRE(reg.Set(reg.Find("hotreload.step"), Arcane::CVarValue::Int32(7), Arcane::SetBy::User, "editor") == Arcane::SetResult::Applied);
    reg.Publish();

    host.Unload();
    const fs::path file = dir / "P" / "Saved" / "Config" / "hotreload.json";
    REQUIRE(fs::exists(file));
    std::ifstream in(file, std::ios::binary);
    CHECK(nlohmann::json::parse(in).at("step") == 7);
    in.close();
    rt.CloseProject();
    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("a callback a module adds from a tick entry point leaves with its image", "[cvar][hotreload]")
{
    // Review Focus 2 (plan RF candidate 2): the fixture's OnFixedUpdate adds a
    // SECOND callback on console.historySize, OUTSIDE load/Init. AddCallback
    // tags it with the innermost CVarModuleScope, so only PluginHost's scope
    // around the FixedUpdate vtable call (ScopedCall) names the module; an
    // untagged add would survive the unload as a dangling function pointer
    // and call into the unmapped image on the next matching Publish.
    RestoreV1();
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    Arcane::Runtime rt(Arcane::Test::Process());
    RegisterFixtureTypes(rt);
    Arcane::PluginHost host(Arcane::Test::Process(), fs::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    host.FixedUpdateAll(1.0 / 60.0);                           // the tick-time AddCallback lands here
    {
        LogCapture log;
        BumpHistorySize(reg);
        // Init's callback AND the tick's: proves the tick-time add landed.
        CHECK(CountOf(log.text, "HotReloadPlugin: console.historySize changed (step 1)") == 2);
    }

    host.Unload();
    REQUIRE(reg.Find("hotreload.step").IsStale());
    {
        LogCapture log;
        BumpHistorySize(reg);                                   // neither callback may run
        CHECK(log.text.find("HotReloadPlugin:") == std::string::npos);
    }
    reg.UnregisterModule("lifetime-test");
    reg.Publish();
}

TEST_CASE("ApplyLayersFor touches only the named module's cvars, and re-applying replaces its records", "[cvar]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_s1_layers_only";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::ofstream(dir / "lay.json", std::ios::binary) << R"({ "mine": 5, "other": 6 })";

    Arcane::CVarRegistry reg;
    const Arcane::CVarHandle mine = reg.Register(Arcane::Test::Desc("lay.mine", Arcane::CVarValue::Int32(0), Arcane::Audience::Game, Arcane::CVarFlags::None, "ModA"));
    const Arcane::CVarHandle other = reg.Register(Arcane::Test::Desc("lay.other", Arcane::CVarValue::Int32(0), Arcane::Audience::Game, Arcane::CVarFlags::None, "ModB"));
    Arcane::LayerSources layers;
    layers.dirs.push_back(Arcane::CVarLayerDir{ Arcane::SetBy::Project, dir, "project" });
    layers.commandLine = { "lay.other=9", "lay.mine=8" };
    reg.ApplyLayersFor("ModA", layers);                       // publishes
    CHECK(reg.Get(mine)->AsInt32() == 8);                     // the command line outranks the Project rung
    CHECK(reg.Get(other)->AsInt32() == 0);                    // ModB's cvar is not touched
    reg.ApplyLayersFor("ModA", layers);
    CHECK(reg.Explain("lay.mine")->history.size() == 3);      // Default, Project, CommandLine -- replaced, not appended
    fs::remove_all(dir, ec);
}

TEST_CASE("a module that loads AFTER the project opened gets its Project rung, and its --set items", "[cvar][hotreload]")
{
    RestoreV1();
    const fs::path dir = ScratchProject("layers");
    { std::ofstream(dir / "P" / "Config" / "hotreload.json", std::ios::binary) << R"({ "step": 42 })"; }
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    Arcane::Runtime rt(Arcane::Test::Process());
    RegisterFixtureTypes(rt);
    REQUIRE(rt.OpenProject(dir / "P"));                       // hotreload.step does not exist yet
    Arcane::PluginHost host(Arcane::Test::Process(), fs::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    CHECK(reg.Get(reg.Find("hotreload.step"))->AsInt32() == 42);
    CHECK(reg.Explain("hotreload.step")->setBy == Arcane::SetBy::Project);
    host.Unload();

    rt.SetCVarCommandLine({ "hotreload.step=77" }, Arcane::CVarContext::Editor);
    REQUIRE(host.Load());
    CHECK(reg.Get(reg.Find("hotreload.step"))->AsInt32() == 77);
    CHECK(reg.Explain("hotreload.step")->setBy == Arcane::SetBy::CommandLine);
    host.Unload();
    rt.SetCVarCommandLine({}, Arcane::CVarContext::Editor);
    rt.CloseProject();
    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("a User value survives a hot reload: flushed before the unload, re-applied from the archive after", "[cvar][hotreload]")
{
    RestoreV1();
    const fs::path dir = ScratchProject("survive");
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    Arcane::Runtime rt(Arcane::Test::Process());
    RegisterFixtureTypes(rt);
    rt.SetUserCVarArchiving(true);
    REQUIRE(rt.OpenProject(dir / "P"));
    Arcane::PluginHost host(Arcane::Test::Process(), fs::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    REQUIRE(reg.Set(reg.Find("hotreload.step"), Arcane::CVarValue::Int32(9), Arcane::SetBy::User, "editor") == Arcane::SetResult::Applied);
    reg.Publish();

    fs::copy_file("HotReloadPluginV2.dll", "HotReloadPluginV1.dll", fs::copy_options::overwrite_existing);
    REQUIRE(host.ForceReload());
    CHECK(reg.Get(reg.Find("hotreload.step"))->AsInt32() == 9);    // V2's default is 10
    CHECK(reg.Explain("hotreload.step")->setBy == Arcane::SetBy::User);
    host.Unload();
    RestoreV1();
    rt.CloseProject();
    std::error_code ec; fs::remove_all(dir, ec);
}

namespace
{
    // A Problems sink that records every publish. RAII, so a failed REQUIRE
    // cannot leave a dangling sink behind for the next case.
    struct DiagCapture
    {
        std::vector<std::pair<std::string, std::vector<Arcane::Diagnostic>>> calls;
        DiagCapture() { Arcane::Diagnostics::SetSink(&Sink, this); }
        ~DiagCapture() { (void)Arcane::Diagnostics::ClearSinkIfCurrent(&Sink, this); }
        static void Sink(std::string_view key, std::span<const Arcane::Diagnostic> diags, void* user)
        {
            static_cast<DiagCapture*>(user)->calls.emplace_back(std::string(key),
                                                                std::vector<Arcane::Diagnostic>(diags.begin(), diags.end()));
        }
        // The newest publish under `key`; nullptr when there was none.
        const std::vector<Arcane::Diagnostic>* Last(std::string_view key) const
        {
            for (auto it = calls.rbegin(); it != calls.rend(); ++it)
                if (it->first == key) return &it->second;
            return nullptr;
        }
    };

    bool NamesKey(const std::vector<Arcane::Diagnostic>& rows, std::string_view key)
    {
        return std::any_of(rows.begin(), rows.end(),
                           [&](const Arcane::Diagnostic& d) { return d.message.find(key) != std::string::npos; });
    }
}

TEST_CASE("a module project's keys are not logged as unknown at OpenProject: the log waits for the module load, then names each surviving issue once", "[cvar][hotreload][diagnostics]")
{
    // S1-30 ruling: the Problems rows are published at OpenProject as the plan
    // says, but the LOG is deferred when the manifest declares a game module --
    // the host loads it AFTER OpenProject, so its keys look unknown until
    // LayerModuleCVars republishes. That publish logs a delta against the last
    // logged set: each surviving issue once, a hot reload nothing.
    RestoreV1();
    const fs::path dir = fs::temp_directory_path() / "arcane_s1_lifetime_diaglog";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "P" / "Config", ec);
    fs::create_directories(dir / "P" / "Content", ec);
    {
        // RuntimeProjectTest's manifest shape, declaring the fixture as the module.
        std::ofstream(dir / "P" / "P.arcproj", std::ios::binary)
            << R"({"formatVersion":)" << Arcane::ProjectManifest::kFormatVersion
            << R"(,"name":"P","engine":{"abi":)" << static_cast<int>(Arcane::kGamePluginABIVersion)
            << R"(},"gameModule":"HotReloadPluginV1.dll","plugins":[],"bootScene":""})";
        std::ofstream(dir / "P" / "Config" / "hotreload.json", std::ios::binary) << R"({ "step": 42 })";
        std::ofstream(dir / "P" / "Config" / "cvardiagtest.json", std::ios::binary) << R"({ "bogus": 1 })";
    }

    DiagCapture cap;
    LogCapture log;
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    Arcane::Runtime rt(Arcane::Test::Process());
    RegisterFixtureTypes(rt);
    REQUIRE(rt.OpenProject(dir / "P"));                       // hotreload.step does not exist yet
    {
        const std::vector<Arcane::Diagnostic>* rows = cap.Last("config.cvars");
        REQUIRE(rows);
        CHECK(rows->size() == 2);                              // the rows are published now ...
        CHECK(NamesKey(*rows, "hotreload.step"));
        CHECK(NamesKey(*rows, "cvardiagtest.bogus"));
        CHECK(log.text.find("cvar config:") == std::string::npos);   // ... the log waits for the module
    }

    Arcane::PluginHost host(Arcane::Test::Process(), fs::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    CHECK(reg.Get(reg.Find("hotreload.step"))->AsInt32() == 42);   // the deferral changed nothing about layering
    {
        const std::vector<Arcane::Diagnostic>* rows = cap.Last("config.cvars");
        REQUIRE(rows);
        REQUIRE(rows->size() == 1);                            // hotreload.step is declared now
        CHECK(NamesKey(*rows, "cvardiagtest.bogus"));
        CHECK(CountOf(log.text, "config.cvar.unknown-key 'cvardiagtest.bogus'") == 1);
        CHECK(CountOf(log.text, "config.cvar.unknown-key 'hotreload.step'") == 0);
    }

    fs::copy_file("HotReloadPluginV2.dll", "HotReloadPluginV1.dll", fs::copy_options::overwrite_existing);
    REQUIRE(host.ForceReload());                               // a second load republishes the same set ...
    {
        const std::vector<Arcane::Diagnostic>* rows = cap.Last("config.cvars");
        REQUIRE(rows);
        CHECK(rows->size() == 1);
        CHECK(CountOf(log.text, "config.cvar.unknown-key 'cvardiagtest.bogus'") == 1);   // ... and logs nothing new
        CHECK(CountOf(log.text, "config.cvar.unknown-key 'hotreload.step'") == 0);
    }

    host.Unload();
    RestoreV1();
    rt.CloseProject();
    fs::remove_all(dir, ec);
}
