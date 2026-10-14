#pragma once

// Two RAII scratch helpers for tests that touch the FILESYSTEM and the
// PROCESS ENVIRONMENT -- lifted out of ToolchainTest.cpp and
// BuildDriverTest.cpp, which each carried a verbatim copy (multibackend
// hardening, whole-branch review F6d). Header-only, Catch2-free: nothing
// here asserts, so it can sit under any test's own REQUIREs.
//
// TempDir: a unique, empty directory under the system temp root, removed on
// scope exit (so a failing assertion cannot strand files for the next run to
// trip on). Unique per INSTANCE (the tag plus this object's own address), and
// wiped before creation in case a crashed earlier run left the same name
// behind -- the union of the two originals' guarantees.
//
// EnvOverride: sets one process environment variable for the scope and
// restores the previous value (or unsets it) on exit. The Toolchain Resolve*
// wrappers and BackendResolver read PATH/PATHEXT/ARCANE_SDK live through
// std::getenv, so proving "nothing on PATH" or "THIS engine checkout is the
// SDK the fixture's premake5.lua includes" needs a real, restored-on-exit
// environment mutation rather than a mock. Not thread-safe (neither is the
// environment); cases that use it are single-threaded.

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Arcane::Test
{
    struct TempDir
    {
        std::filesystem::path path;

        explicit TempDir(std::string_view tag)
            : path(std::filesystem::temp_directory_path() / "arcane_test_scratch" /
                   (std::string(tag) + "_" +
                    std::to_string(static_cast<unsigned>(
                        std::hash<const void*>{}(this)))))
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        TempDir(const TempDir&)            = delete;
        TempDir& operator=(const TempDir&) = delete;
    };

    class EnvOverride
    {
    public:
        EnvOverride(const char* name, const std::string& value)
            : name_(name)
        {
            if (const char* existing = std::getenv(name))
                previous_ = existing;
            Set(value);
        }

        ~EnvOverride()
        {
            Set(previous_.value_or(std::string()));
        }

        EnvOverride(const EnvOverride&)            = delete;
        EnvOverride& operator=(const EnvOverride&) = delete;

    private:
        void Set(const std::string& value)
        {
#ifdef _WIN32
            // An empty value REMOVES the variable (documented _putenv_s
            // behaviour) -- exactly "unset" when there was no previous value.
            _putenv_s(name_.c_str(), value.c_str());
#else
            if (value.empty())
                unsetenv(name_.c_str());
            else
                setenv(name_.c_str(), value.c_str(), 1);
#endif
        }

        std::string                name_;
        std::optional<std::string> previous_;
    };
}
