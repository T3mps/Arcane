// Settings spec s4.4 (O1/O2): a module's cvars, commands and callbacks live
// exactly as long as its image, and the config rungs reach them when it
// (re)loads. Uses the HotReloadPlugin fixtures beside ArcaneTests.exe -- run
// FROM the exe dir. [cvar][hotreload]

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Project/Project.hpp>

#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

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
