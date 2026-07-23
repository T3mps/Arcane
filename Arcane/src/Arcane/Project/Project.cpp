#include <Arcane/Project/Project.hpp>

#include <Arcane/Base/Log.hpp>   // ARC_WARN (confirmed path, see Task 3)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion

#include <Json.hpp>

#include <fstream>
#include <system_error>

namespace Arcane
{
    std::optional<Project> Project::Open(const std::filesystem::path& pathOrFile)
    {
        std::error_code ec;

        std::filesystem::path root;
        std::filesystem::path manifestFile;

        if (std::filesystem::is_regular_file(pathOrFile, ec)
            && pathOrFile.extension() == ".arcproj")
        {
            manifestFile = pathOrFile;
            root         = pathOrFile.parent_path();
        }
        else if (std::filesystem::is_directory(pathOrFile, ec))
        {
            root = pathOrFile;
            // Find exactly one *.arcproj in the folder.
            for (const auto& entry : std::filesystem::directory_iterator(pathOrFile, ec))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".arcproj")
                {
                    if (!manifestFile.empty())
                    {
                        ARC_WARN("Project::Open: multiple .arcproj in '{}'", pathOrFile.generic_string());
                        return std::nullopt;   // ambiguous
                    }
                    manifestFile = entry.path();
                }
            }
        }

        if (manifestFile.empty())
        {
            ARC_WARN("Project::Open: no .arcproj at '{}'", pathOrFile.generic_string());
            return std::nullopt;
        }

        auto manifest = ProjectManifest::LoadFile(manifestFile);
        if (!manifest)
            return std::nullopt;   // LoadFile already logged

        Project proj;
        proj.m_root     = root;
        proj.m_manifest = std::move(*manifest);
        // Default mounts + the asset identity map (Guid -> mount path). game:// is the
        // project's own Content/; plugin content folds in below. (engine:// stays reserved
        // until the engine ships built-in content -- spec Q2.)
        proj.m_mounts.Mount("game", root / "Content");
        proj.m_registry.ScanContent(root / "Content", "game");

        // Slice 4 -- project plugins. Each enabled plugin in the manifest lives at
        // <root>/Plugins/<name>/ with a <name>.arcplugin descriptor (same shape as
        // .arcproj). Its Content/ (if any) mounts as "plugin/<name>://" and its assets
        // join the SAME registry, so a plugin asset is addressed by its Guid exactly like
        // a game asset. Dependency direction stays one-way (project -> plugin -> engine).
        // (Loading a plugin's Source/-built DLL through the host is a follow-up; the host
        // loads one game module today. Engine-plugin discovery is reserved -- spec Q2.)
        for (const auto& ref : proj.m_manifest.plugins)
        {
            if (!ref.enabled)
                continue;

            const std::filesystem::path pluginRoot = root / "Plugins" / ref.name;
            const std::filesystem::path descriptor = pluginRoot / (ref.name + ".arcplugin");
            if (!std::filesystem::is_regular_file(descriptor, ec))
            {
                ARC_WARN("Project::Open: plugin '{}' is enabled but has no descriptor at '{}'",
                         ref.name, descriptor.generic_string());
                continue;
            }
            // Validate the descriptor (well-formed .arcproj-shaped manifest). LoadFile logs
            // on failure; a malformed descriptor disables the plugin rather than the project.
            if (!ProjectManifest::LoadFile(descriptor))
                continue;

            const std::filesystem::path content = pluginRoot / "Content";
            if (std::filesystem::is_directory(content, ec))
            {
                const std::string scheme = "plugin/" + ref.name;   // -> "plugin/<name>://<rel>"
                proj.m_mounts.Mount(scheme, content);
                proj.m_registry.AddContent(content, scheme);
            }
        }

        return proj;
    }

    std::optional<Project> Project::Create(const std::filesystem::path& dir, std::string name)
    {
        std::error_code ec;

        // Reject a bad name BEFORE touching disk: empty, or containing a path
        // separator, would otherwise let the manifest filename (dir / (name +
        // ".arcproj")) write outside `dir`, or silently produce a nameless file.
        if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
        {
            ARC_WARN("Project::Create: invalid project name '{}'", name);
            return std::nullopt;
        }

        // Refuse a non-empty existing dir (avoid clobbering); an absent dir is fine.
        if (std::filesystem::exists(dir, ec) && !std::filesystem::is_empty(dir, ec))
        {
            ARC_WARN("Project::Create: target '{}' exists and is not empty", dir.generic_string());
            return std::nullopt;
        }

        for (const char* sub : { "Source", "Content", "Config", "Plugins" })
        {
            std::filesystem::create_directories(dir / sub, ec);
            if (ec)
            {
                ARC_WARN("Project::Create: mkdir '{}' failed: {}", (dir / sub).generic_string(), ec.message());
                return std::nullopt;
            }
        }

        // Minimal manifest. ABI comes from the engine's plugin-ABI constant so a freshly
        // created project always targets the engine that created it. Built via nlohmann
        // so `name` is escaped correctly (rather than hand-concatenated into JSON text).
        nlohmann::json manifestJson;
        manifestJson["formatVersion"] = 1;
        manifestJson["name"]          = name;
        manifestJson["engine"]        = { { "abi", static_cast<int>(Arcane::kGamePluginABIVersion) } };
        manifestJson["gameModule"]    = "";
        manifestJson["plugins"]       = nlohmann::json::array();
        manifestJson["bootScene"]     = "";

        const std::filesystem::path manifestPath = dir / (name + ".arcproj");
        {
            std::ofstream out(manifestPath, std::ios::binary);
            out << manifestJson.dump(2);
            if (!out.good())
            {
                ARC_WARN("Project::Create: failed writing manifest '{}'", manifestPath.generic_string());
                return std::nullopt;
            }
        }

        static const char* kGitignore =
            "# Derived / generated -- never commit\n"
            "Binaries/\n"
            "Intermediate/\n"
            "Saved/\n"
            "\n"
            "# Plugin generated output\n"
            "Plugins/**/Binaries/\n"
            "Plugins/**/Intermediate/\n"
            "\n"
            "# Generated IDE project files\n"
            "*.sln\n"
            "*.vcxproj\n"
            "*.vcxproj.filters\n"
            "*.vcxproj.user\n"
            ".vs/\n";

        const std::filesystem::path gitignorePath = dir / ".gitignore";
        {
            std::ofstream out(gitignorePath, std::ios::binary);
            out << kGitignore;
            if (!out.good())
            {
                ARC_WARN("Project::Create: failed writing '{}'", gitignorePath.generic_string());
                return std::nullopt;
            }
        }

        return Open(dir);
    }

    std::optional<std::filesystem::path> Project::ResolveAsset(const AssetId& id) const
    {
        if (!id.IsValid())
            return std::nullopt;
        // Guid -> mount path (AssetRegistry) -> physical file (MountTable).
        auto mountPath = m_registry.Resolve(id.Value());
        if (!mountPath)
            return std::nullopt;
        return m_mounts.Resolve(*mountPath);
    }
}
