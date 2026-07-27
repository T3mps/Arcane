#pragma once

// Project: an opened Arcane project -- its manifest, root directory, and mount table.
// Open() discovers/loads a .arcproj and registers default mounts; ResolveAsset() is the
// asset-reference seam (AssetId -> filesystem path via the mounts). Create() scaffolds a
// new project skeleton on disk.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/AssetRegistry.hpp>
#include <Arcane/Project/MountTable.hpp>
#include <Arcane/Project/ProjectManifest.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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
        const AssetRegistry&         Registry() const { return m_registry; }

        // The roots (<root>/Plugins/<name>) of the plugins Open() actually activated: those
        // that were manifest-enabled AND had a valid descriptor. The single source of truth
        // for "which plugins are live" -- content mounts key off the same gate, and callers
        // (config layering, host module loading) should iterate THIS, not the raw manifest
        // list (which still lists enabled-but-invalid plugins). In manifest order.
        const std::vector<std::filesystem::path>& ActivePluginRoots() const { return m_activePluginRoots; }

        // The asset-reference seam: AssetId -> physical file. Slice 1 resolves via the
        // MountTable (AssetId backing = a logical mount path). Slice 2 reroutes this
        // through the AssetRegistry (GUID) WITHOUT changing the signature.
        std::optional<std::filesystem::path> ResolveAsset(const AssetId& id) const;

        // Register a file CREATED after the open-time scan (editor New Material /
        // New Instance / save) into the registry: finds which content root contains
        // it (game://, then each active plugin's Content) and registers it under
        // that scheme. Idempotent. nullopt (warn) when the file lies outside every
        // content root -- such a file cannot resolve by GUID by design.
        std::optional<Guid> RegisterAsset(const std::filesystem::path& file);

        // Point this project's bootScene at `id` (nil clears it), rewriting the
        // .arcproj in place. False on read/parse/write failure, leaving both the
        // file and the in-memory manifest untouched.
        //
        // The Guid, NOT a mount path: the AssetRegistry rebuilds its Guid ->
        // mount-path map on every open, so a scene that moves on disk keeps
        // working. Written atomically (temp + rename) so an interrupted write
        // cannot leave a project with a truncated manifest.
        bool SetBootScene(const Guid& id);

    private:
        // Open()/Create() are the only construction paths (they are static members and
        // can reach this). std::optional<Project> in Runtime move-constructs, never
        // default-constructs, so a private default ctor is safe.
        Project() = default;

        std::filesystem::path m_root;
        std::filesystem::path m_manifestFile;   // the .arcproj this project was opened from
        ProjectManifest       m_manifest;
        MountTable            m_mounts;
        AssetRegistry         m_registry;
        std::vector<std::filesystem::path> m_activePluginRoots;   // valid + enabled plugins, in manifest order
    };
}
