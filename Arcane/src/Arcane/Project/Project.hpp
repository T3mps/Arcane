#pragma once

// Project: an opened Arcane project -- its manifest, root directory, and mount table.
// Open() discovers/loads a .arcproj and registers default mounts; ResolveAsset() is the
// asset-reference seam (AssetId -> filesystem path via the mounts). Create() scaffolds a
// new project skeleton on disk.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/MountTable.hpp>
#include <Arcane/Project/ProjectManifest.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace Arcane
{
    class ARCANE_API Project
    {
    public:
        // Open a project folder (finds the single *.arcproj inside) or a direct
        // .arcproj file. Loads the manifest and mounts game:// -> <root>/Content.
        // nullopt on: no/multiple .arcproj, or an invalid manifest.
        static std::optional<Project> Open(const std::filesystem::path& pathOrFile);

        // Scaffold a new project at `dir` named `name`: creates the folder skeleton
        // (Source/ Content/ Config/ Plugins/), writes <name>.arcproj and .gitignore,
        // then opens it. nullopt if `dir` exists non-empty or on IO error.
        static std::optional<Project> Create(const std::filesystem::path& dir, std::string name);

        const ProjectManifest&       Manifest() const { return m_manifest; }
        const std::filesystem::path& Root()     const { return m_root; }
        const MountTable&            Mounts()   const { return m_mounts; }

        // The asset-reference seam: AssetId -> physical file. Slice 1 resolves via the
        // MountTable (AssetId backing = a logical mount path). Slice 2 reroutes this
        // through the AssetRegistry (GUID) WITHOUT changing the signature.
        std::optional<std::filesystem::path> ResolveAsset(const AssetId& id) const;

    private:
        // Open()/Create() are the only construction paths (they are static members and
        // can reach this). std::optional<Project> in Runtime move-constructs, never
        // default-constructs, so a private default ctor is safe.
        Project() = default;

        std::filesystem::path m_root;
        ProjectManifest       m_manifest;
        MountTable            m_mounts;
    };
}
