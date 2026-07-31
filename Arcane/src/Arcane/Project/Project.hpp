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
        //
        // `onProgress`, when non-empty, is forwarded to the primary game://
        // content scan (AssetRegistry::ScanContent) -- see that method's own
        // comment for the cost/contract. Not forwarded to each plugin's
        // AddContent below: that folding is comparatively small, and the
        // primary Content/ scan is what a boot splash's "Scanning content..."
        // line is about.
        static std::optional<Project> Open(const std::filesystem::path& pathOrFile,
                                           AssetRegistry::ScanProgressFn onProgress = {});

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
        // working. Written atomically via a temp file plus a platform atomic
        // replace (see Project.cpp for the per-platform mechanism) so an
        // interrupted write cannot leave a project with a truncated manifest.
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

    // EditorLock: "which process has this project open", answered from the
    // project itself so it survives launcher restarts. The editor writes
    // <root>/Saved/editor.lock while it holds a project (boot, project
    // switch) and clears it on clean exit; the Arcane Hub reads it to focus
    // the running editor instead of spawning a rival, and to show running
    // badges after ITS process restarted.
    //
    // Unity's lockfile model, minus the flaw its users hate: the lock names
    // {pid, process CREATION time}, and IsAlive() only believes it when that
    // exact pid with that exact start time still runs -- so a crash's stale
    // lock fails validation and is simply ignored, never a false "already
    // open". MIRRORED FORMAT: the Hub parses the same JSON in its
    // editorlock.rs; change BOTH or hub-side detection goes blind.
    namespace EditorLock
    {
        struct Info
        {
            uint32_t pid = 0;
            uint64_t start = 0;   // process creation FILETIME as u64 (0 on non-Windows)
        };

        ARCANE_API std::filesystem::path FileFor(const std::filesystem::path& projectRoot);

        // Pure halves, exported so the format is pinned by tests.
        ARCANE_API std::string ToJson(const Info& info);
        ARCANE_API std::optional<Info> Parse(const std::string& text);

        // THIS process's identity. start is 0 where the platform query fails.
        ARCANE_API Info Self();

        // Write/clear the lock for a project root. Best-effort: a lock that
        // cannot be written must not fail a project open (WARN only).
        ARCANE_API void Write(const std::filesystem::path& projectRoot);
        ARCANE_API void Clear(const std::filesystem::path& projectRoot);

        // Read + validate: Some(pid) only when the named process is STILL the
        // process the lock described (pid alive AND creation time matches).
        ARCANE_API std::optional<uint32_t> ReadLive(const std::filesystem::path& projectRoot);

        // ReadLive minus ourselves: the pid of ANOTHER live editor holding this
        // project, or nullopt. The direct-launch guard rides this -- the editor
        // refuses to double-open a rival's project (main.cpp boot gate, exit 3;
        // SwitchProject refusal) -- and the self-exemption is what lets a
        // same-project re-open proceed over our own lock.
        ARCANE_API std::optional<uint32_t> RivalPid(const std::filesystem::path& projectRoot);

        // Bring the first visible top-level window of `pid` to the foreground.
        // MIRROR of the Hub's spawn.rs focus_process_window: the honest answer
        // to "already open" is to surface the editor that has it. False when
        // the process has no visible window yet (mid-boot) -- callers still
        // refuse; the user gets the message instead of the window.
        ARCANE_API bool FocusWindowOfProcess(uint32_t pid);
    }
}
