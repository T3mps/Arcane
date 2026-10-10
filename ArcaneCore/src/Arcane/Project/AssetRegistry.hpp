#pragma once

// AssetRegistry: the one map that matters -- Guid -> mount path ("game://a/b.json") --
// built by scanning a project's content tree at open. The physical file then comes from
// the MountTable (mount path -> file), so an asset can move on disk while its Guid (the
// stable reference) never changes. Where the Guid lives splits by asset kind (Unity's
// model): native JSON assets carry a top-level "id"; imported binary originals
// (.png/.wav/.ttf/...) that cannot embed one keep it in a sibling "<file>.meta" sidecar.
// In both cases a missing/invalid id is minted and written back (Unity-style auto-import).
//
// One exception to both rules: .arcdiag crash/hang/gpu-stall reports (GPU crash
// diagnostics arc, Task 9) carry an embedded guid too, but under a DIFFERENT top-level
// key ("guid", DiagEnvelope.hpp's Envelope -- never "id") and are NEVER minted or written
// back here, since a report's identity is fixed the moment Diagnostics wrote it. See
// AddFile's .arcdiag branch (AssetRegistry.cpp).
//
// And a fourth rule for C/C++ source files (.cpp/.hpp/.h/...; Source/ in the Asset
// Browser): the guid is DERIVED from the mount path (Guid::FromName), never minted and
// never written -- no sidecar may ever land in a source tree. Same path, same guid,
// across restarts; a rename is a new identity. See AddFile's IsSourceFile branch. A
// non-source file under the "source" scheme (a .json/.png/... sharing Source/ with the
// code) is ignored outright -- never read, minted, or written -- since source:// is a
// listing of code, not an import root; see AddFile's scheme == "source" guard.

#include <Arcane/Core/Api.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Util/FunctionRef.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Arcane
{
    // T5 s7.2: why AssetRegistry::Rebind refused (Ok = the mapping moved). NoProject
    // is Runtime::RebindMovedAsset's answer when nothing is open.
    enum class RebindResult : std::uint8_t
    {
        Ok, UnknownGuid, NotTrackable, OutsideContent, CrossMount, IdMismatch, PathTaken, NoProject,
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // unordered_map member on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARC_CORE_API AssetRegistry
    {
    public:
        // Sub-progress a caller can observe while ScanContent walks a (possibly
        // large) content tree: (files scanned so far, total files that will be
        // scanned). FunctionRef, not std::function -- ScanContent calls it
        // SYNCHRONOUSLY, once per file, from within the scan loop and never
        // stores it, so a non-owning view is the right (zero-allocation) tool;
        // see FunctionRef.hpp's own contract. Default-constructed (empty) means
        // "no observer" -- ScanContent still does its ordinary single-pass work
        // in that case (see ScanContent's own comment for the cost this adds
        // ONLY when a callback is supplied).
        using ScanProgressFn = Arcane::FunctionRef<void(std::size_t done, std::size_t total)>;

        // Clear the registry, then scan `contentDir` for assets -- native JSON (embedded
        // "id") and imported binary originals (id in a sibling "<file>.meta") -- assigning
        // or reading a stable Guid per asset, and register Guid -> "<scheme>://<relative>".
        // A file with no valid id gets one generated and written back. Returns the number
        // of assets registered. This is Clear() + AddContent(); a full rebuild from scratch.
        //
        // `onProgress`, when non-empty, is called after each regular file is
        // processed with (files done, total files under contentDir) -- done is
        // monotonic and the final call always has done == total. Getting that
        // total needs a cheap stat-only directory_iterator pass BEFORE the real
        // scan (no file reads, just counting), so it is only paid when a caller
        // actually wants progress: with no callback this delegates straight to
        // AddContent, identical cost to before this parameter existed.
        //
        // TOCTOU caveat: the two passes are not atomic. A tree that mutates
        // BETWEEN them (a file added/removed while scanning) can make `done`
        // under- or overshoot `total`, so the "final call always has done ==
        // total" guarantee above holds for a STATIC tree only. Harmless for
        // every caller today -- an open-time project scan of a directory
        // nothing else is touching -- but would need revisiting before ever
        // pointing this at a live/watched directory.
        std::size_t ScanContent(const std::filesystem::path& contentDir, std::string_view scheme,
                                ScanProgressFn onProgress = {});

        // Scan another content root into the SAME registry WITHOUT clearing it first --
        // used to fold a mounted plugin's (or engine's) content in beside game:// content,
        // each under its own scheme ("plugin/<name>", "engine"). Returns the number of
        // assets newly registered by THIS call (duplicates keep the first + warn).
        std::size_t AddContent(const std::filesystem::path& contentDir, std::string_view scheme);

        // Register ONE file under `scheme`, relative to `contentDir` -- the incremental
        // sibling of AddContent for assets created AFTER the open-time scan (the editor's
        // New Material / New Instance / save flows). Same kind rules as the scan (native
        // embedded id / imported-binary sidecar; ids minted + written back when missing).
        // Idempotent for an already-registered mapping. nullopt when the file is not a
        // trackable asset kind, cannot be read, or lies outside `contentDir`.
        std::optional<Guid> AddFile(const std::filesystem::path& file,
                                    const std::filesystem::path& contentDir,
                                    std::string_view scheme);

        // T5 (spec 2026-09-30 s7.2): the id a scan WOULD read for `file` -- embedded
        // "id" (native JSON), "<file>.meta" "guid" (imported binary), envelope "guid"
        // (.arcdiag) -- but NEVER minted, written or warned about. nullopt for a missing
        // file, a .meta, C/C++ source (its id is its path), an unknown kind, or an
        // unreadable/absent id. The asset file-op executor's TOCTOU and expiry probe.
        static std::optional<Guid> PeekId(const std::filesystem::path& file);

        // T5 s7.2: drop `id`'s mapping (its file was deleted by the editor), retract
        // its DiagLocator::Asset rows and republish "assets" whole-set. False if unknown.
        bool Remove(const Guid& id);

        // T5 s7.2: point `id` at `newFile` after the editor MOVED it (AddFile would
        // keep the old path). Never mints, writes or publishes; the map is untouched
        // unless the result is Ok. Checks, in order: unknown id; source (identity is
        // the path); outside `contentDir`; `scheme` != the id's current mount;
        // PeekId(newFile) != id; the mount path owned by another guid.
        RebindResult Rebind(const Guid& id, const std::filesystem::path& newFile,
                            const std::filesystem::path& contentDir, std::string_view scheme);

        // Guid -> mount path ("game://a/b.json"); nullopt if the id is unknown.
        std::optional<std::string> Resolve(const Guid& id) const;

        // Snapshot of every (guid, mount path) pair -- the asset browser's feed.
        // Ordered: ascending mount path, Guid breaking ties, so the sequence is
        // total and a function of content alone -- never of hash or insertion order.
        std::vector<std::pair<Guid, std::string>> All() const;

        std::size_t Count() const { return m_byGuid.size(); }
        void        Clear()       { m_byGuid.clear(); }

    private:
        std::unordered_map<Guid, std::string> m_byGuid;   // guid -> mount path

        // KEY OWNERSHIP: this registry owns the fixed Diagnostics key "assets" --
        // see AssetRegistry.cpp for the accumulate-then-publish-once shape. A
        // member (not a local in ScanContent) because AddContent is also called
        // standalone, after ScanContent, to fold plugin content into the SAME
        // registry (Project::Open) -- if each such call published its OWN local
        // vector, a later plugin's clean AddContent would silently retract an
        // earlier plugin's (or the game scan's) still-true diagnostics. Cleared
        // only where m_byGuid is cleared (ScanContent's full-rebuild entry).
        std::vector<Diagnostic> m_scanDiagnostics;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
