#include <Arcane/Platform/Paths.hpp>

#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>   // _getpid
#else
#include <unistd.h>    // getpid
#endif

namespace Arcane::Paths
{
    namespace
    {
        std::mutex& StateMutex() { static std::mutex m; return m; }
        Config& State() { static Config c; return c; }

        // An environment directory, read fresh every call (tests change it);
        // empty when unset or blank.
        std::filesystem::path EnvDir(const char* name)
        {
#if defined(_WIN32)
            const std::wstring wide(name, name + std::char_traits<char>::length(name));
            if (const wchar_t* v = _wgetenv(wide.c_str()); v && *v) return std::filesystem::path(v);
#else
            if (const char* v = std::getenv(name); v && *v) return std::filesystem::path(v);
#endif
            return {};
        }

        // Per-user, machine-local data and config bases (XDG on Linux).
        std::filesystem::path LocalData()
        {
#if defined(_WIN32)
            return EnvDir("LOCALAPPDATA");
#else
            if (std::filesystem::path x = EnvDir("XDG_DATA_HOME"); !x.empty()) return x;
            if (std::filesystem::path h = EnvDir("HOME"); !h.empty()) return h / ".local" / "share";
            return {};
#endif
        }

        std::filesystem::path LocalConfig()
        {
#if defined(_WIN32)
            return EnvDir("LOCALAPPDATA");
#else
            if (std::filesystem::path x = EnvDir("XDG_CONFIG_HOME"); !x.empty()) return x;
            if (std::filesystem::path h = EnvDir("HOME"); !h.empty()) return h / ".config";
            return {};
#endif
        }

        std::string ProcessTag()
        {
#if defined(_WIN32)
            return std::to_string(static_cast<long>(_getpid()));
#else
            return std::to_string(static_cast<long>(::getpid()));
#endif
        }

        bool HasProject(const Config& c) { return c.projectDir.has_value() && !c.projectDir->empty(); }

        std::filesystem::path Below(const std::filesystem::path& base, const std::filesystem::path& rel)
        {
            return base.empty() ? base : base / rel;
        }
    }

    void Configure(const Config& config)
    {
        const std::lock_guard<std::mutex> lock(StateMutex());
        State() = config;
    }

    Config Current()
    {
        const std::lock_guard<std::mutex> lock(StateMutex());
        return State();
    }

    Config ForProject(const std::filesystem::path& projectRoot)
    {
        Config c = Current();
        c.projectDir = projectRoot;
        return c;
    }

    std::filesystem::path UserRoot()
    {
        return Below(LocalData(), "Arcane");
    }

    std::filesystem::path Resolve(Location location, const Config& c)
    {
        const bool project = HasProject(c);
        const std::filesystem::path root = project ? *c.projectDir : std::filesystem::path{};
        switch (location)
        {
        case Location::EngineDir:           return c.engineDir;
        case Location::EngineData:          return Below(c.engineDir, "data");
        case Location::EngineConfig:        return Below(Below(c.engineDir, "data"), "EngineConfig");
        case Location::ProjectDir:          return root;
        case Location::ProjectConfig:       return Below(root, "Config");
        case Location::ProjectContent:      return Below(root, "Content");
        case Location::ProjectSaved:        return c.dist ? std::filesystem::path{} : Below(root, "Saved");
        case Location::ProjectIntermediate: return c.dist ? std::filesystem::path{} : Below(root, "Intermediate");
        case Location::ProjectCache:        return c.dist ? std::filesystem::path{} : Below(Below(root, "Saved"), "Cache");
        case Location::EditorUserDir:       return c.dist ? std::filesystem::path{} : Below(UserRoot(), "Editor");
        case Location::GameUserDir:
        {
            if (!c.dist) return Below(root, "Saved");
            const std::filesystem::path base = LocalConfig();
            if (base.empty() || c.gameName.empty()) return {};
            std::filesystem::path p = base;
            if (!c.companyName.empty()) p /= c.companyName;
            return p / c.gameName;
        }
        case Location::DiagnosticsDir:
            if (c.dist) return Below(Resolve(Location::GameUserDir, c), "Diagnostics");
            if (project) return root / "Saved" / "Diagnostics";
            return Below(c.engineDir, "diagnostics");
        case Location::TempDir:
        {
            std::error_code ec;
            const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
            return ec ? std::filesystem::path{} : tmp / "Arcane" / ProcessTag();
        }
        }
        return {};
    }

    std::filesystem::path Join(Location location, const Config& config, const std::filesystem::path& rel)
    {
        return Below(Resolve(location, config), rel);
    }

    std::filesystem::path Get(Location location)
    {
        return Resolve(location, Current());
    }

    std::filesystem::path EnsureDir(Location location)
    {
        const Config c = Current();
        const std::filesystem::path dir = Resolve(location, c);
        if (dir.empty()) return dir;
        const bool readOnly = location == Location::EngineDir || location == Location::EngineData
                           || location == Location::EngineConfig
                           || (c.dist && (location == Location::ProjectDir || location == Location::ProjectConfig
                                          || location == Location::ProjectContent));
        if (!readOnly)
        {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
        }
        return dir;
    }
}
