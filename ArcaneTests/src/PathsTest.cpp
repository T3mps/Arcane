// Arcane::Paths (settings spec s11.0): every well-known location, per build
// type. Windows-only resolution checks (the desk); the Linux XDG branch is named
// in Paths.cpp for the port. The last two cases pin the Core consumers: Runtime
// keeps Paths in step with the engine dir and the open project, and PluginHost
// stages its images under TempDir. [paths]

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Project/Project.hpp>

#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"

#include <filesystem>
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
    CHECK(Resolve(L::GameUserDir, noGame).empty());
    CHECK(Resolve(L::DiagnosticsDir, noGame).empty());
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
