// S6-3: log.dir, log.file.* and log.server.* are settings with the pre-sweep
// defaults; the engine log file (Log.cpp), Diagnostics' log directory and the
// header-only server Logger (Util/Logger.hpp) read them.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"

#include <Arcane/Util/Logger.hpp>   // first: it defines SPDLOG_ACTIVE_LEVEL before spdlog is included

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/DiagnosticsSettings.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/LogFileSettings.hpp>
#include <Arcane/Config/Bindings/LogBinding.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Util/LogServerSettings.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

using namespace Arcane;

namespace
{
    // Drops this test's Code records on the named cvars and publishes, on
    // every exit path.
    struct ClearCodeRungs
    {
        std::vector<const char*> names;
        ~ClearCodeRungs()
        {
            CVarRegistry& reg = CVarRegistry::Get();
            for (const char* name : names)
                reg.ClearRung(reg.Find(name), SetBy::Code);
            reg.PublishImmediate();
        }
    };

    void SetCode(std::string_view name, const CVarValue& value)
    {
        CVarRegistry& reg = CVarRegistry::Get();
        const CVarHandle h = reg.Find(name);
        INFO("cvar " << std::string(name));
        REQUIRE_FALSE(h.IsStale());
        REQUIRE(reg.Set(h, value, SetBy::Code) == SetResult::Applied);
    }

    std::string ReadAll(const std::filesystem::path& file)
    {
        std::ifstream in(file);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    // A fresh, empty directory under TEMP. A file sink a previous run left
    // open is not ours to close, so a failed remove is not an error.
    std::filesystem::path FreshDir(const char* leaf)
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / leaf;
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        return dir;
    }
}

TEST_CASE("sweep: log.file / log.server defaults are the pre-sweep literals", "[sweep][log]")
{
    STATIC_REQUIRE(std::is_same_v<decltype(LogFileSettings{}.keepCount), std::int32_t>);
    STATIC_REQUIRE(std::is_same_v<decltype(LogServerFileSettings{}.maxBytes), std::uint64_t>);
    CHECK(LogFileSettings{}.keepCount == 5);
    CHECK(LogFileSettings{}.flushLevel == 3);                  // spdlog::level::warn
    CHECK(LogServerSettings{}.consoleLevel == 2);              // info
    CHECK(LogServerSettings{}.fileLevel == 0);                 // trace
    CHECK(LogServerSettings{}.flushLevel == 2);                // info
    CHECK(LogServerSettings{}.pattern == "%^[%H:%M:%S.%e] [%n] [%l]%$ %v");
    CHECK(LogServerSettings{}.filePattern == "[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
    CHECK(LogServerFileSettings{}.maxBytes == 5ull * 1024 * 1024);
    CHECK(LogServerFileSettings{}.maxFiles == 3);
    CHECK(LogSettings{}.dir.empty());
    Test::RequireDefault("log.file.keepCount", CVarValue::Int32(5));
    Test::RequireDefault("log.file.flushLevel", CVarValue::Int32(3));
    Test::RequireDefault("log.server.consoleLevel", CVarValue::Int32(2));
    Test::RequireDefault("log.server.fileLevel", CVarValue::Int32(0));
    Test::RequireDefault("log.server.flushLevel", CVarValue::Int32(2));
    Test::RequireDefault("log.server.pattern", CVarValue::String("%^[%H:%M:%S.%e] [%n] [%l]%$ %v"));
    Test::RequireDefault("log.server.filePattern", CVarValue::String("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v"));
    Test::RequireDefault("log.server.file.maxBytes", CVarValue::UInt64(5ull << 20));
    Test::RequireDefault("log.server.file.maxFiles", CVarValue::Int32(3));
    Test::RequireDefault("log.dir", CVarValue::String(""));
}

TEST_CASE("sweep: the log rows carry the frozen inventory's audience, scope and apply", "[sweep][log]")
{
    struct Row { const char* name; Audience audience; SettingScope scope; ApplyMode apply; bool dev; };
    const Row rows[] = {
        { "log.dir",                  Audience::Game,   SettingScope::PreferencesProject, ApplyMode::Restart, true  },
        { "log.file.keepCount",       Audience::Game,   SettingScope::PreferencesProject, ApplyMode::Restart, true  },
        { "log.file.flushLevel",      Audience::Game,   SettingScope::PreferencesProject, ApplyMode::Live,    true  },
        { "log.server.consoleLevel",  Audience::Server, SettingScope::Project,            ApplyMode::Live,    false },
        { "log.server.fileLevel",     Audience::Server, SettingScope::Project,            ApplyMode::Live,    false },
        { "log.server.pattern",       Audience::Server, SettingScope::Project,            ApplyMode::Live,    true  },
        { "log.server.filePattern",   Audience::Server, SettingScope::Project,            ApplyMode::Live,    true  },
        { "log.server.flushLevel",    Audience::Server, SettingScope::Project,            ApplyMode::Live,    false },
        { "log.server.file.maxBytes", Audience::Server, SettingScope::Project,            ApplyMode::Restart, false },
        { "log.server.file.maxFiles", Audience::Server, SettingScope::Project,            ApplyMode::Restart, false },
    };
    for (const Row& row : rows)
    {
        INFO("cvar " << row.name);
        const auto e = CVarRegistry::Get().Explain(row.name);
        REQUIRE(e.has_value());
        CHECK(e->audience == row.audience);
        CHECK(e->scope == row.scope);
        CHECK(e->apply == row.apply);
        CHECK(HasFlag(e->flags, CVarFlags::Dev) == row.dev);
        CHECK_FALSE(e->help.empty());
    }
}

TEST_CASE("sweep: log rotation keeps log.file.keepCount files", "[sweep][log]")
{
    namespace fs = std::filesystem;
    ClearCodeRungs restore{ { "log.file.keepCount" } };
    const fs::path dir = FreshDir("arcane-sweep-log-rotate");
    const fs::path file = dir / "t.log";
    Log::Init();   // a no-op when an earlier test already initialised the engine logger

    // A stray from a run with a larger keepCount is pruned too, even past a
    // gap; a file that is not <stem>.<n>.log is not ours.
    std::ofstream(dir / "t.4.log") << "stray";
    std::ofstream(dir / "t.old.log") << "keep me";
    SetCode("log.file.keepCount", CVarValue::Int32(2));
    CVarRegistry::Get().PublishImmediate();
    for (int i = 0; i < 4; ++i)
    {
        std::ofstream(file) << i;
        REQUIRE(Log::AttachFileSink(file));
    }
    CHECK(fs::exists(dir / "t.1.log"));
    CHECK(fs::exists(dir / "t.2.log"));
    CHECK_FALSE(fs::exists(dir / "t.3.log"));
    CHECK_FALSE(fs::exists(dir / "t.4.log"));
    CHECK(fs::exists(dir / "t.old.log"));
    CHECK(ReadAll(dir / "t.1.log") == "3");
    CHECK(ReadAll(dir / "t.2.log") == "2");

    // 0 keeps no history: the previous file is deleted, not rotated.
    SetCode("log.file.keepCount", CVarValue::Int32(0));
    CVarRegistry::Get().PublishImmediate();
    std::ofstream(file) << "gone";
    REQUIRE(Log::AttachFileSink(file));
    CHECK_FALSE(fs::exists(dir / "t.1.log"));
    CHECK_FALSE(fs::exists(dir / "t.2.log"));
}

TEST_CASE("sweep: log.file.flushLevel sets the engine logger's flush level, live", "[sweep][log]")
{
    ClearCodeRungs restore{ { "log.file.flushLevel" } };
    const std::filesystem::path dir = FreshDir("arcane-sweep-log-flush");
    Log::Init();
    REQUIRE(Log::AttachFileSink(dir / "f.log"));
    CHECK(Log::Engine()->flush_level() == spdlog::level::warn);

    SetCode("log.file.flushLevel", CVarValue::Int32(1));
    CVarRegistry::Get().PublishImmediate();
    CHECK(Log::Engine()->flush_level() == spdlog::level::debug);

    // A re-attach (a project switch retargets the sink) reads the live value too.
    REQUIRE(Log::AttachFileSink(dir / "f.log"));
    CHECK(Log::Engine()->flush_level() == spdlog::level::debug);

    CVarRegistry::Get().ClearRung(CVarRegistry::Get().Find("log.file.flushLevel"), SetBy::Code);
    CVarRegistry::Get().PublishImmediate();
    CHECK(Log::Engine()->flush_level() == spdlog::level::warn);
}

TEST_CASE("sweep: log.dir reaches Diagnostics::Config through ConfigFromSettings", "[sweep][log]")
{
    ClearCodeRungs restore{ { "log.dir" } };
    CHECK(Diagnostics::ConfigFromSettings(DiagnosticsSettings{}).logDir.empty());
    SetCode("log.dir", CVarValue::String("D:/logs-here"));
    CVarRegistry::Get().PublishImmediate();
    CHECK(Settings<LogSettings>().dir == "D:/logs-here");
    CHECK(Diagnostics::ConfigFromSettings(DiagnosticsSettings{}).logDir == "D:/logs-here");
}

TEST_CASE("sweep: the server Logger reads log.server.* at Init and follows the Live rows", "[sweep][log][logger]")
{
    ClearCodeRungs restore{ { "log.server.consoleLevel", "log.server.fileLevel", "log.server.flushLevel",
                              "log.server.filePattern" } };
    struct ShutdownLogger { ~ShutdownLogger() { Logger::Shutdown(); } } shutdown;
    const std::filesystem::path dir = FreshDir("arcane-sweep-server-log");
    const std::filesystem::path file = dir / "server.log";
    Logger::Shutdown();   // a fresh Init below, whatever an earlier test left

    SetCode("log.server.consoleLevel", CVarValue::Int32(4));
    SetCode("log.server.fileLevel", CVarValue::Int32(1));
    SetCode("log.server.flushLevel", CVarValue::Int32(0));
    SetCode("log.server.filePattern", CVarValue::String("SWEEP|%l|%v"));
    CVarRegistry::Get().PublishImmediate();

    Logger::Init(Level::Info, Level::Trace, "");     // explicit arguments still win
    spdlog::logger* probe = Logger::Get("SweepProbe");
    REQUIRE(probe->sinks().size() == 1);
    CHECK(probe->sinks()[0]->level() == spdlog::level::info);
    Logger::Shutdown();

    Logger::Init(std::nullopt, std::nullopt, file.string());   // no level given: the settings decide
    probe = Logger::Get("SweepProbe");
    REQUIRE(probe->sinks().size() == 2);
    CHECK(probe->sinks()[0]->level() == spdlog::level::err);
    CHECK(probe->sinks()[1]->level() == spdlog::level::debug);
    CHECK(probe->flush_level() == spdlog::level::trace);
    probe->info("hello sweep");
    probe->flush();
    CHECK(ReadAll(file).find("SWEEP|info|hello sweep") != std::string::npos);

    // Live: a publish re-applies the levels to the running logger.
    SetCode("log.server.consoleLevel", CVarValue::Int32(1));
    SetCode("log.server.flushLevel", CVarValue::Int32(3));
    CVarRegistry::Get().PublishImmediate();
    CHECK(probe->sinks()[0]->level() == spdlog::level::debug);
    CHECK(probe->flush_level() == spdlog::level::warn);
}
