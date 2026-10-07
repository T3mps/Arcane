#pragma once

// Where a project's per-user locations resolve in THIS build, and the suite's
// private per-user root (settings S7-DIST).
//
// The locations go through the product's own resolver (PathsConfigFor +
// Paths, as Runtime's UserCVarDir does): a dev build keeps them in the
// project's Saved/; a Dist build keeps them in the per-user OS dir named by
// the project's company and game (settings S7, spec s8.2, s11.0).
//
// In Dist that per-user dir is the player's profile, so test_main installs a
// PrivateUserDataRoot before anything resolves a path: this process and its
// children read LOCALAPPDATA (XDG_DATA_HOME and XDG_CONFIG_HOME off Windows)
// as a folder under TEMP, removed when the run ends. A Dist run never writes
// the real profile. ScopedUserDataBase redirects the same bases for one case.

#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectOpenOptions.hpp>
#include <Arcane/Project/ProjectPaths.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>   // _getpid
#include <stdlib.h>    // _wputenv_s
#else
#include <unistd.h>    // getpid
#endif

namespace Arcane::Test
{
    // The Paths config for the project at `root`, with the identity from its
    // manifest. Opens the project without the diag:// mount: an identity
    // read, nothing else.
    inline Paths::Config ProjectPathsConfig(const std::filesystem::path& root)
    {
        ProjectOpenOptions opts;
        opts.mountDiagnostics = false;
        const std::optional<Project> project = Project::Open(root, {}, opts);
        INFO("project " << root.string());
        REQUIRE(project.has_value());
        const Paths::Config current = Paths::Current();
        return PathsConfigFor(*project, current.engineDir, kDistBuild || current.dist);
    }

    // The User cvar rung's folder for the project at `root`.
    inline std::filesystem::path UserConfigDir(const std::filesystem::path& root)
    {
        return Paths::Join(Paths::Location::GameUserDir, ProjectPathsConfig(root), "Config");
    }

    // UserConfigDir, emptied. In Dist every project with one game name shares
    // that folder, so a case that did not empty it would read an earlier
    // case's archive. REQUIREs a folder (never a relative path).
    inline std::filesystem::path FreshUserConfigDir(const std::filesystem::path& root)
    {
        const std::filesystem::path dir = UserConfigDir(root);
        REQUIRE_FALSE(dir.empty());
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return dir;
    }

    // The project's DiagnosticsDir: <root>/Saved/Diagnostics in dev, the
    // per-user <game>/Diagnostics in Dist.
    inline std::filesystem::path DiagnosticsDirFor(const std::filesystem::path& root)
    {
        return Paths::Resolve(Paths::Location::DiagnosticsDir, ProjectPathsConfig(root));
    }

    // The OS per-user bases Paths reads: LOCALAPPDATA on Windows; XDG_DATA_HOME
    // (UserRoot) and XDG_CONFIG_HOME (a Dist GameUserDir) elsewhere.
    using EnvString = std::filesystem::path::string_type;
#if defined(_WIN32)
    inline constexpr const wchar_t* kUserDataEnv[] = { L"LOCALAPPDATA" };
    inline std::optional<EnvString> GetUserDataEnv(const wchar_t* name)
    {
        if (const wchar_t* v = _wgetenv(name)) return EnvString(v);
        return std::nullopt;
    }
    inline void SetUserDataEnv(const wchar_t* name, const std::optional<EnvString>& value)
    {
        _wputenv_s(name, value ? value->c_str() : L"");   // L"" removes it
    }
#else
    inline constexpr const char* kUserDataEnv[] = { "XDG_DATA_HOME", "XDG_CONFIG_HOME" };
    inline std::optional<EnvString> GetUserDataEnv(const char* name)
    {
        if (const char* v = std::getenv(name)) return EnvString(v);
        return std::nullopt;
    }
    inline void SetUserDataEnv(const char* name, const std::optional<EnvString>& value)
    {
        if (value) ::setenv(name, value->c_str(), 1);
        else ::unsetenv(name);
    }
#endif

    // Points the per-user bases at `base` for the scope (this process and the
    // children it starts), then restores them.
    class ScopedUserDataBase
    {
    public:
        explicit ScopedUserDataBase(const std::filesystem::path& base)
        {
            for (std::size_t i = 0; i < std::size(kUserDataEnv); ++i)
            {
                m_saved[i] = GetUserDataEnv(kUserDataEnv[i]);
                SetUserDataEnv(kUserDataEnv[i], base.native());
            }
        }
        ~ScopedUserDataBase()
        {
            for (std::size_t i = 0; i < std::size(kUserDataEnv); ++i)
                SetUserDataEnv(kUserDataEnv[i], m_saved[i]);
        }
        ScopedUserDataBase(const ScopedUserDataBase&) = delete;
        ScopedUserDataBase& operator=(const ScopedUserDataBase&) = delete;

    private:
        std::optional<EnvString> m_saved[std::size(kUserDataEnv)];
    };

    struct SuiteUserData
    {
        Paths::PlatformDirs real;            // the bases the process started with
        std::filesystem::path privateRoot;   // empty unless a PrivateUserDataRoot is live
    };

    inline SuiteUserData& SuiteUserDataState()
    {
        static SuiteUserData state;
        return state;
    }

    // Dist only: a dev build keeps game data in the project's Saved/. Points
    // the per-user bases at <TEMP>/arcane-tests-userdata-<pid> for the whole
    // run (ScopedUserDataBase) and removes that folder on destruction.
    class PrivateUserDataRoot
    {
    public:
        PrivateUserDataRoot()
        {
            if (!kDistBuild) return;
            SuiteUserData& state = SuiteUserDataState();
            state.real = Paths::CurrentPlatformDirs();
            std::error_code ec;
#if defined(_WIN32)
            const std::string tag = std::to_string(_getpid());
#else
            const std::string tag = std::to_string(::getpid());
#endif
            const std::filesystem::path root = std::filesystem::temp_directory_path(ec) / ("arcane-tests-userdata-" + tag);
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(root, ec);
            m_base.emplace(root);
            state.privateRoot = root;
        }

        ~PrivateUserDataRoot()
        {
            SuiteUserData& state = SuiteUserDataState();
            if (state.privateRoot.empty()) return;
            m_base.reset();
            std::error_code ec;
            std::filesystem::remove_all(state.privateRoot, ec);
            state.privateRoot.clear();
        }

        PrivateUserDataRoot(const PrivateUserDataRoot&) = delete;
        PrivateUserDataRoot& operator=(const PrivateUserDataRoot&) = delete;

    private:
        std::optional<ScopedUserDataBase> m_base;
    };
}
