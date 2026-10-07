// Arcane::Paths (settings spec s11.0): every well-known location, per build
// type. Windows-only resolution checks (the desk); the Linux XDG branch is named
// in Paths.cpp for the port. The last two cases pin the Core consumers: Runtime
// keeps Paths in step with the engine dir and the open project, and PluginHost
// stages its images under TempDir. [paths]

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Host/EarlyConfig.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectPaths.hpp>   // ApplyEngineDirDefaults, kDistBuild

#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <cstdlib>
#include <process.h>
#endif

namespace
{
    namespace fs = std::filesystem;
    using L = Arcane::Paths::Location;

    bool Same(const fs::path& a, const fs::path& b)
    {
        return a.lexically_normal().generic_string() == b.lexically_normal().generic_string();
    }

    struct ScopedPathsConfig
    {
        Arcane::Paths::Config saved = Arcane::Paths::Current();
        ~ScopedPathsConfig() { Arcane::Paths::Configure(saved); }
    };

    void WriteText(const fs::path& p, const std::string& text)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary) << text;
    }

    fs::path FreshDir(const char* name)
    {
        const fs::path dir = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(dir, ec);
        return dir;
    }

    // A cvar an engine-config folder sets (S2-H items 1 and 3). Teardown
    // restores the engine dir, then builds one Runtime so the EngineConfig
    // rung is back on the exe dir's folder for the tests that follow.
    struct EngineRungProbe
    {
        Arcane::Paths::Config saved = Arcane::Paths::Current();
        Arcane::CVarHandle knob;
        EngineRungProbe()
        {
            Arcane::CVarDesc desc;
            desc.name = "s2hengine.knob";
            desc.type = Arcane::CVarType::Int32;
            desc.defaultValue = Arcane::CVarValue::Int32(1);
            desc.help = "S2-H engine rung probe.";
            desc.module = "s2h-engine-rung-test";
            knob = Arcane::CVarRegistry::Get().Register(desc);
        }
        ~EngineRungProbe()
        {
            Arcane::CVarRegistry::Get().UnregisterModule("s2h-engine-rung-test");
            Arcane::Paths::Configure(saved);
            const Arcane::Runtime reset(Arcane::Test::Process());
        }
        EngineRungProbe(const EngineRungProbe&) = delete;
        EngineRungProbe& operator=(const EngineRungProbe&) = delete;
        std::int32_t Value() const { return Arcane::CVarRegistry::Get().Get(knob)->AsInt32(); }
        std::size_t EngineRecords() const
        {
            const auto explained = Arcane::CVarRegistry::Get().Explain("s2hengine.knob");
            return static_cast<std::size_t>(std::count_if(explained->history.begin(), explained->history.end(),
                [](const Arcane::CVarHistoryRecord& h) { return h.by == Arcane::SetBy::EngineConfig; }));
        }
        static void UseEngineDir(const fs::path& engineDir)
        {
            Arcane::Paths::Config paths = Arcane::Paths::Current();
            paths.engineDir = engineDir;
            Arcane::Paths::Configure(paths);
        }
    };

#if defined(_WIN32)
    struct ScopedLocalAppData
    {
        std::wstring saved;
        bool had = false;
        explicit ScopedLocalAppData(const wchar_t* value)
        {
            if (const wchar_t* v = _wgetenv(L"LOCALAPPDATA")) { saved = v; had = true; }
            _wputenv_s(L"LOCALAPPDATA", value);   // L"" removes it
        }
        ~ScopedLocalAppData() { _wputenv_s(L"LOCALAPPDATA", had ? saved.c_str() : L""); }
    };
#endif

    Arcane::Paths::Config Sample(bool dist, bool project)
    {
        Arcane::Paths::Config c;
        c.engineDir = "C:/Engine/bin";
        if (project) c.projectDir = fs::path("C:/Proj");
        c.companyName = "Studio";
        c.gameName = "MyGame";
        c.dist = dist;
        return c;
    }
}

#if defined(_WIN32)
TEST_CASE("Paths: an editor/dev session with a project", "[paths]")
{
    const ScopedLocalAppData lad(L"C:\\ArcaneTest\\Local");
    const Arcane::Paths::Config c = Sample(false, true);
    using Arcane::Paths::Resolve;
    CHECK(Same(Resolve(L::EngineDir, c), "C:/Engine/bin"));
    CHECK(Same(Resolve(L::EngineData, c), "C:/Engine/bin/data"));
    CHECK(Same(Resolve(L::EngineConfig, c), "C:/Engine/bin/data/EngineConfig"));
    CHECK(Same(Resolve(L::ProjectDir, c), "C:/Proj"));
    CHECK(Same(Resolve(L::ProjectConfig, c), "C:/Proj/Config"));
    CHECK(Same(Resolve(L::ProjectContent, c), "C:/Proj/Content"));
    CHECK(Same(Resolve(L::ProjectSaved, c), "C:/Proj/Saved"));
    CHECK(Same(Resolve(L::ProjectIntermediate, c), "C:/Proj/Intermediate"));
    CHECK(Same(Resolve(L::ProjectCache, c), "C:/Proj/Saved/Cache"));
    CHECK(Same(Resolve(L::EditorUserDir, c), "C:/ArcaneTest/Local/Arcane/Editor"));
    CHECK(Same(Resolve(L::GameUserDir, c), "C:/Proj/Saved"));                  // dev runs: the project's Saved/
    CHECK(Same(Resolve(L::DiagnosticsDir, c), "C:/Proj/Saved/Diagnostics"));
    CHECK(Same(Resolve(L::TempDir, c), fs::temp_directory_path() / "Arcane" / std::to_string(_getpid())));
    CHECK(Same(Arcane::Paths::UserRoot(), "C:/ArcaneTest/Local/Arcane"));
    CHECK(Same(Arcane::Paths::Join(L::ProjectSaved, c, "UndoCache"), "C:/Proj/Saved/UndoCache"));
}

TEST_CASE("Paths: a dev session with no project leaves every project location empty", "[paths]")
{
    const ScopedLocalAppData lad(L"C:\\ArcaneTest\\Local");
    const Arcane::Paths::Config c = Sample(false, false);
    for (const L loc : { L::ProjectDir, L::ProjectConfig, L::ProjectContent, L::ProjectSaved, L::ProjectIntermediate, L::ProjectCache, L::GameUserDir })
        CHECK(Arcane::Paths::Resolve(loc, c).empty());
    CHECK(Same(Arcane::Paths::Resolve(L::DiagnosticsDir, c), "C:/Engine/bin/diagnostics"));   // today's <exe dir>/diagnostics
    CHECK(Arcane::Paths::Join(L::ProjectSaved, c, "UndoCache").empty());                      // never a relative path
}

TEST_CASE("Paths: a Dist game keeps user data under %LOCALAPPDATA%\\<Company>\\<Game> and has no editor or Saved/", "[paths]")
{
    const ScopedLocalAppData lad(L"C:\\ArcaneTest\\Local");
    const Arcane::Paths::Config c = Sample(true, true);
    using Arcane::Paths::Resolve;
    CHECK(Same(Resolve(L::ProjectConfig, c), "C:/Proj/Config"));                // packaged with the game
    CHECK(Same(Resolve(L::ProjectContent, c), "C:/Proj/Content"));
    CHECK(Resolve(L::ProjectSaved, c).empty());
    CHECK(Resolve(L::ProjectIntermediate, c).empty());
    CHECK(Resolve(L::ProjectCache, c).empty());
    CHECK(Resolve(L::EditorUserDir, c).empty());
    CHECK(Same(Resolve(L::GameUserDir, c), "C:/ArcaneTest/Local/Studio/MyGame"));
    CHECK(Same(Resolve(L::DiagnosticsDir, c), "C:/ArcaneTest/Local/Studio/MyGame/Diagnostics"));
    Arcane::Paths::Config noCompany = c;
    noCompany.companyName.clear();
    CHECK(Same(Resolve(L::GameUserDir, noCompany), "C:/ArcaneTest/Local/MyGame"));
    Arcane::Paths::Config noGame = c;
    noGame.gameName.clear();
    CHECK(Same(Resolve(L::GameUserDir, noGame), "C:/ArcaneTest/Local/Studio/ArcaneGame"));
    CHECK(Same(Resolve(L::DiagnosticsDir, noGame), "C:/ArcaneTest/Local/Studio/ArcaneGame/Diagnostics"));
}

TEST_CASE("Paths: with LOCALAPPDATA unset, per-user locations are empty, never relative", "[paths]")
{
    const ScopedLocalAppData lad(L"");
    CHECK(Arcane::Paths::UserRoot().empty());
    CHECK(Arcane::Paths::Resolve(L::EditorUserDir, Sample(false, true)).empty());
    CHECK(Arcane::Paths::Resolve(L::GameUserDir, Sample(true, true)).empty());
    CHECK(Arcane::Paths::Join(L::EditorUserDir, Sample(false, true), "layouts").empty());
}
#endif

TEST_CASE("Paths: Configure/Current/Get/ForProject round-trip; Get creates nothing, EnsureDir creates only writable locations", "[paths]")
{
    const ScopedPathsConfig restore;
    const fs::path root = fs::temp_directory_path() / "arcane_paths_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    Arcane::Paths::Config c;
    c.engineDir = root / "engine";
    c.projectDir = root / "proj";
    c.gameName = "RoundTrip";
    Arcane::Paths::Configure(c);
    CHECK(Arcane::Paths::Current().gameName == "RoundTrip");
    CHECK(Same(Arcane::Paths::Get(L::ProjectSaved), root / "proj" / "Saved"));
    CHECK_FALSE(fs::exists(root / "proj" / "Saved"));
    CHECK(Same(Arcane::Paths::EnsureDir(L::ProjectSaved), root / "proj" / "Saved"));
    CHECK(fs::is_directory(root / "proj" / "Saved"));
    (void)Arcane::Paths::EnsureDir(L::EngineConfig);
    CHECK_FALSE(fs::exists(root / "engine"));                                  // read-only: never created
    const Arcane::Paths::Config other = Arcane::Paths::ForProject(root / "other");
    CHECK(Same(*other.projectDir, root / "other"));
    CHECK(Same(other.engineDir, root / "engine"));
    CHECK(Same(*Arcane::Paths::Current().projectDir, root / "proj"));          // ForProject changes nothing global
    fs::remove_all(root, ec);
}

TEST_CASE("Runtime keeps Paths in step: the engine dir at construction, the project on open, cleared on close and on destruction", "[paths][project]")
{
    const ScopedPathsConfig restore;
    Arcane::Paths::Configure(Arcane::Paths::Config{});         // a fresh process's state
    const fs::path dir = fs::temp_directory_path() / "arcane_paths_runtime";
    std::error_code ec;
    fs::remove_all(dir, ec);
    REQUIRE(Arcane::Project::Create(dir / "A", "Alpha").has_value());

    Arcane::Runtime rt(Arcane::Test::Process());
    const fs::path exeDir = fs::path(Arcane::ExecutablePathUtf8()).parent_path();
    CHECK(Same(Arcane::Paths::Get(L::EngineDir), exeDir));
    CHECK(Same(Arcane::Paths::Get(L::EngineConfig), exeDir / "data" / "EngineConfig"));
    REQUIRE(rt.OpenProject(dir / "A"));
    CHECK(Same(Arcane::Paths::Get(L::ProjectDir), rt.CurrentProject()->Root()));
    CHECK(Arcane::Paths::Current().gameName == "Alpha");
    CHECK(Same(Arcane::Paths::Get(L::GameUserDir) / "Config", rt.CurrentProject()->Root() / "Saved" / "Config"));   // the User rung's home, unchanged
    rt.CloseProject();
    CHECK(Arcane::Paths::Get(L::ProjectDir).empty());
    CHECK(Arcane::Paths::Current().gameName.empty());
    {
        Arcane::Runtime scoped(Arcane::Test::Process());
        REQUIRE(scoped.OpenProject(dir / "A"));
        CHECK_FALSE(Arcane::Paths::Get(L::ProjectDir).empty());
    }
    CHECK(Arcane::Paths::Get(L::ProjectDir).empty());           // a Runtime that dies with its project open forgets it
    fs::remove_all(dir, ec);
}

// S2-H item 1: ONE engine-config directory. A host that configured the engine
// dir before the first Runtime gets its EngineConfig rung from that dir on a
// cold boot, and CVarLayerSources -- what a module reload re-layers from --
// names the same folder.
TEST_CASE("Runtime: a host-configured engine dir is the EngineConfig rung's one folder, cold boot and reload alike", "[paths][cvar]")
{
    const fs::path engine = FreshDir("arcane_s2h_engine_dir");
    WriteText(engine / "data" / "EngineConfig" / "s2hengine.json", R"({ "knob": 5 })");
    const EngineRungProbe probe;
    REQUIRE_FALSE(probe.knob.IsStale());
    EngineRungProbe::UseEngineDir(engine);
    {
        Arcane::Runtime rt(Arcane::Test::Process());
        CHECK(probe.Value() == 5);                                         // the cold boot read the host's folder
        const Arcane::LayerSources layers = rt.CVarLayerSources();
        REQUIRE_FALSE(layers.dirs.empty());
        CHECK(layers.dirs.front().by == Arcane::SetBy::EngineConfig);
        CHECK(Same(layers.dirs.front().dir, engine / "data" / "EngineConfig"));   // ...and a reload re-reads the same one
    }
    std::error_code ec;
    fs::remove_all(engine, ec);
}

// S2-H item 3: the EngineConfig rung is applied once per process (per folder),
// not re-read and re-published by every Runtime a host builds. A module reload
// still re-layers it through ApplyLayersFor, and a new folder replaces the old.
TEST_CASE("Runtime: the EngineConfig rung is applied once per folder, not by every Runtime", "[paths][cvar]")
{
    const fs::path engine = FreshDir("arcane_s2h_engine_once");
    const fs::path other = FreshDir("arcane_s2h_engine_other");
    const fs::path file = engine / "data" / "EngineConfig" / "s2hengine.json";
    WriteText(file, R"({ "knob": 5 })");
    const EngineRungProbe probe;
    REQUIRE_FALSE(probe.knob.IsStale());
    Arcane::CVarRegistry& cvars = Arcane::CVarRegistry::Get();
    EngineRungProbe::UseEngineDir(engine);
    {
        const Arcane::Runtime first(Arcane::Test::Process());
        CHECK(probe.Value() == 5);
    }
    WriteText(file, R"({ "knob": 7 })");
    const std::uint64_t serial = cvars.Snapshot()->serial;
    {
        Arcane::Runtime second(Arcane::Test::Process());
        CHECK(cvars.Snapshot()->serial == serial);                          // no re-read, no publish
        CHECK(probe.Value() == 5);
        cvars.ApplyLayersFor("s2h-engine-rung-test", second.CVarLayerSources());   // a module reload re-layers
        CHECK(probe.Value() == 7);
        CHECK(probe.EngineRecords() == 1);
    }
    EngineRungProbe::UseEngineDir(other);                                   // no s2hengine.json there
    {
        const Arcane::Runtime third(Arcane::Test::Process());
        CHECK(probe.Value() == 1);                                          // the old folder's records left with it
        CHECK(probe.EngineRecords() == 0);
    }
    std::error_code ec;
    fs::remove_all(engine, ec);
    fs::remove_all(other, ec);
}

// S2-H item 1 on the HostBoot path (fix round 1): a host that configured the
// engine dir before HostBoot::ApplyEarlyConfigRungs gets its EngineConfig rung
// from that ONE folder -- a key only <exe dir>/data/EngineConfig sets is never
// layered -- and the first Runtime after HostBoot neither re-reads the folder
// nor re-publishes (item 3).
TEST_CASE("HostBoot: a host-configured engine dir is the early EngineConfig rung's one folder, and the first Runtime keeps it", "[paths][cvar]")
{
    const fs::path engine = FreshDir("arcane_s2h_engine_hostboot");
    WriteText(engine / "data" / "EngineConfig" / "s2hengine.json", R"({ "knob": 5 })");
    const EngineRungProbe probe;
    REQUIRE_FALSE(probe.knob.IsStale());
    Arcane::CVarRegistry& cvars = Arcane::CVarRegistry::Get();
    Arcane::CVarDesc desc;
    desc.name = "s2hexeonly.knob";
    desc.type = Arcane::CVarType::Int32;
    desc.defaultValue = Arcane::CVarValue::Int32(1);
    desc.help = "S2-H probe a key only the exe dir's engine-config folder sets.";
    desc.module = "s2h-engine-rung-test";                                   // the probe unregisters it
    const Arcane::CVarHandle exeOnly = cvars.Register(desc);
    REQUIRE_FALSE(exeOnly.IsStale());
    // Declared after the probe, so the file (and the folder, when this test
    // made it) is gone before its teardown Runtime re-applies the exe dir's.
    struct RemoveOnExit
    {
        fs::path file;
        bool     madeDir = !fs::exists(file.parent_path());
        ~RemoveOnExit()
        {
            std::error_code ec;
            fs::remove(file, ec);
            if (madeDir)
                fs::remove(file.parent_path(), ec);
        }
    } const exeFile{ fs::path(Arcane::ExecutablePathUtf8()).parent_path() / "data" / "EngineConfig" / "s2hexeonly.json" };
    WriteText(exeFile.file, R"({ "knob": 9 })");

    EngineRungProbe::UseEngineDir(engine);
    Arcane::HostConfig cfg{};                                                // no project, no --set
    Arcane::HostBoot::ApplyEarlyConfigRungs(cfg, Arcane::CommandLineCVarContext(), /*editor*/ false);
    CHECK(probe.Value() == 5);                                               // the host's folder...
    CHECK(cvars.Get(exeOnly)->AsInt32() == 1);                               // ...and not the exe dir's
    CHECK(probe.EngineRecords() == 1);
    const std::uint64_t serial = cvars.Snapshot()->serial;
    {
        const Arcane::Runtime first(Arcane::Test::Process());
        CHECK(cvars.Snapshot()->serial == serial);                          // not re-read, not re-published
        CHECK(probe.Value() == 5);
        CHECK(cvars.Get(exeOnly)->AsInt32() == 1);
        CHECK(probe.EngineRecords() == 1);
    }
    std::error_code ec;
    fs::remove_all(engine, ec);
}

// S7-9 fix round 1 (spec s8.2, s11.1): the defaults the EngineConfig rung
// fills into Paths. The exe dir only when no host named an engine dir; a Dist
// build's `dist` even under a host's engine dir (HostBoot's early User rung
// resolves GameUserDir from Current() right after); a dev build never clears a
// `dist` a host or test set. Pure, so the Dist branch is pinned from dev too.
TEST_CASE("ApplyEngineDirDefaults: exe dir only when unset; a Dist build sets dist under a host's engine dir; dev never clears it", "[paths]")
{
    const fs::path exe = "X:/exe";
    for (const bool distBuild : { false, true })
    {
        CAPTURE(distBuild);
        Arcane::Paths::Config fresh;                                         // a fresh process
        CHECK(Arcane::ApplyEngineDirDefaults(fresh, exe, distBuild));
        CHECK(fresh.engineDir == exe);
        CHECK(fresh.dist == distBuild);
        CHECK_FALSE(Arcane::ApplyEngineDirDefaults(fresh, exe, distBuild));  // idempotent

        for (const bool preset : { false, true })
        {
            CAPTURE(preset);
            Arcane::Paths::Config host;                                      // a host configured Paths first
            host.engineDir = "H:/host-engine";
            host.projectDir = fs::path("P:/proj");
            host.companyName = "Starworks";
            host.gameName = "Aphelyon";
            host.dist = preset;
            CHECK(Arcane::ApplyEngineDirDefaults(host, exe, distBuild) == (distBuild && !preset));
            CHECK(host.engineDir == fs::path("H:/host-engine"));             // the host's engine dir is kept
            CHECK(host.dist == (distBuild || preset));                       // Dist sets it; dev never clears it
            REQUIRE(host.projectDir);
            CHECK(*host.projectDir == fs::path("P:/proj"));
            CHECK(host.companyName == "Starworks");
            CHECK(host.gameName == "Aphelyon");
        }
    }
}

// The same seam through the real call: a host-configured engine dir survives
// ApplyEngineConfigRung, and Paths' dist is this build's (or the preset).
// EngineRungProbe restores Paths and re-applies the exe dir's rung after.
TEST_CASE("ApplyEngineConfigRung keeps a host-configured engine dir and sets dist == (kDistBuild || preset)", "[paths][cvar]")
{
    const fs::path engine = FreshDir("arcane_s7_engine_dist");
    WriteText(engine / "data" / "EngineConfig" / "s2hengine.json", R"({ "knob": 4 })");
    const EngineRungProbe probe;
    REQUIRE_FALSE(probe.knob.IsStale());
    for (const bool preset : { false, true })
    {
        CAPTURE(preset);
        Arcane::Paths::Config paths = Arcane::Paths::Current();
        paths.engineDir = engine;
        paths.dist = preset;
        Arcane::Paths::Configure(paths);
        (void)Arcane::ApplyEngineConfigRung();
        Arcane::CVarRegistry::Get().PublishImmediate();
        CHECK(Same(Arcane::Paths::Current().engineDir, engine));                         // untouched
        CHECK(Arcane::Paths::Current().dist == (Arcane::kDistBuild || preset));
        CHECK(Same(Arcane::Paths::Get(L::EngineConfig), engine / "data" / "EngineConfig"));
        CHECK(probe.Value() == 4);                                                       // the host's folder is the rung
    }
    std::error_code ec;
    fs::remove_all(engine, ec);
}

TEST_CASE("PluginHost stages its versioned module copies under Paths' TempDir", "[paths][hotreload]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Arcane::HotReloadTest::Pulse>();
    rt.Components()->RegisterComponent<Arcane::HotReloadTest::RoleCounters>();
    Arcane::PluginHost host(Arcane::Test::Process(), fs::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    CHECK(fs::exists(Arcane::Paths::Get(L::TempDir) / "plugins" / "HotReloadPluginV1_1.dll"));
    host.Unload();
}
