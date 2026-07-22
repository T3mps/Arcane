#include <Arcane/Project/Project.hpp>

#include <Arcane/Base/Log.hpp>   // ARC_WARN (confirmed path, see Task 3)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion

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
        // Default mounts. engine:// and plugin://<name>/ are registered in later slices.
        proj.m_mounts.Mount("game", root / "Content");
        return proj;
    }

    std::optional<Project> Project::Create(const std::filesystem::path& dir, std::string name)
    {
        std::error_code ec;

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
        // created project always targets the engine that created it.
        const std::string manifest =
            "{\n"
            "  \"formatVersion\": 1,\n"
            "  \"name\": \"" + name + "\",\n"
            "  \"engine\": { \"abi\": " + std::to_string(Arcane::kGamePluginABIVersion) + " },\n"
            "  \"gameModule\": \"\",\n"
            "  \"plugins\": [],\n"
            "  \"bootScene\": \"\"\n"
            "}\n";
        std::ofstream(dir / (name + ".arcproj"), std::ios::binary) << manifest;

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
        std::ofstream(dir / ".gitignore", std::ios::binary) << kGitignore;

        return Open(dir);
    }

    std::optional<std::filesystem::path> Project::ResolveAsset(const AssetId& id) const
    {
        if (!id.IsValid())
            return std::nullopt;
        // Slice 1: the AssetId key IS the logical mount path. Slice 2 replaces this body
        // with an AssetRegistry lookup (GUID -> mount path) before the mount resolve.
        return m_mounts.Resolve(id.Key());
    }
}
