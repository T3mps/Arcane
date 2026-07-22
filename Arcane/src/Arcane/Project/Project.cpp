#include <Arcane/Project/Project.hpp>

#include <Arcane/Base/Log.hpp>   // ARC_WARN (confirmed path, see Task 3)

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

    std::optional<std::filesystem::path> Project::ResolveAsset(const AssetId& id) const
    {
        if (!id.IsValid())
            return std::nullopt;
        // Slice 1: the AssetId key IS the logical mount path. Slice 2 replaces this body
        // with an AssetRegistry lookup (GUID -> mount path) before the mount resolve.
        return m_mounts.Resolve(id.Key());
    }
}
