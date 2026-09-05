#pragma once

// Arcane::AssetPipeline::CookSession -- F2b Task 5: the shared cook orchestration both
// arccook (this task's CLI, arccook/src/main.cpp) and the editor's in-process cook
// (Task 12) drive VERBATIM, so the CLI and the editor never diverge on what "stale" or
// "cooked" means. A CookSession owns no state across calls beyond the last CookProject
// call's failure map -- every CookProject/CheckProject call opens its own
// Arcane::AssetPipeline::ArtifactStore rooted at <projectDir>/Intermediate and reads the
// project's Content/ tree fresh, so a session is cheap to construct once per process
// (arccook's main.cpp) or reuse across many calls (a future editor cook button).
//
// Source enumeration: every ".png" under <projectDir>/Content (recursive) that has a
// sibling "<file>.png.meta" sidecar (Unity convention -- matches
// ArcaneClient/src/Arcane/Project/AssetRegistry.cpp's ResolveSidecarId) is a texture
// source. A ".png" with no sidecar is not yet a registered asset -- CookSession never
// mints one (minting a guid is AssetRegistry's job, host-side only) -- it is silently
// skipped, never a failure. Nothing outside Content/ is ever considered: in particular
// ReferenceProject/Verify/References/*.png (the golden-image ORACLE) is never under
// Content/ and must never cook.
//
// Staleness: a source is up to date iff an artifact already exists at
// ArtifactStore::PathFor(ComputeCookKey(currentBytes, currentSettings,
// kTextureImporterVersion)) -- a PURE, content-addressed check that needs no in-memory
// index and is correct even the very first time this process has ever seen the project.
// CheckProject uses exactly this check, and nothing else, to answer `anyStale`.
//
// Failure memoization: when TextureImporter::ImportTexture fails (corrupt/unrecognised
// source bytes) for a given cook key, the reason is persisted to
// <projectDir>/Intermediate/Failed/<hex16>.fail (same hash-keyed spirit as
// ArtifactStore, deliberately simpler -- one flat file per key, no 256-way sharding,
// because failures are rare relative to artifacts). A later CookProject call -- same OR
// a different CookSession/process, sharing the same Intermediate/ tree -- for the SAME
// cook key reads that memo and reports the failure again WITHOUT ever re-invoking the
// importer: the "no retry storm" contract. The memo is itself keyed by the cook key
// (source bytes + settings + importer version), so it self-invalidates the moment any of
// those three change -- a fixed/edited source or a settings change computes a NEW cook
// key with no memo, and is attempted again. ArtifactStore::Commit failures (rare IO
// errors, not the source's fault) are deliberately NOT memoized -- they are retried
// every call, since the cost of retrying a commit is one file write, not a decode+encode.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Arcane/AssetPipeline/TextureImporter.hpp"
#include "Arcane/Guid.hpp"

namespace Arcane::AssetPipeline
{
    struct CookResult
    {
        std::size_t cooked = 0;      // freshly imported + committed this call
        std::size_t upToDate = 0;    // already had a current artifact for that key
        std::size_t failed = 0;      // import failed (fresh or memoized) or commit failed

        // F2b Task 12: WHICH guids landed in the `cooked` bucket this call --
        // the editor's background cook queue needs this to invalidate exactly
        // the textures that changed (the Assets facade's per-guid memos, the
        // NriTextureCache GPU residency entry, the mesh-albedo bindless slot
        // memo), rather than sweeping every known guid in the project on
        // every cook pass. `arccook`'s CLI and CookSession's own [pipeline]
        // tests never read this -- purely additive, existing callers that
        // only look at the three counts above are unaffected.
        std::vector<Guid> cookedGuids;

        // Guid -> human reason, for every guid that landed in the `failed`
        // bucket this call -- the SAME strings LastFailures() carries,
        // duplicated into the result itself (not read back out of
        // LastFailures() later) so a caller that consumes CookResult on a
        // DIFFERENT thread than the one that called CookProject (the
        // editor's cook queue runs CookProject on a JobSystem worker and
        // drains the result on the main thread) never races a LATER
        // CookProject call already reusing the same CookSession/
        // m_lastFailures.
        std::vector<std::pair<Guid, std::string>> failures;
    };

    class CookSession
    {
    public:
        // Fires once per ATTEMPTED source (never for a skipped, sidecar-less ".png"):
        // `source` is the source file's path, `ok` is true for a fresh cook OR an
        // up-to-date hit, false for any failure (fresh or memoized); `detail` is a
        // short, human-readable reason on false (empty on true). Optional -- arccook's
        // --verbose wires this to stdout. CheckProject never calls it: it never
        // attempts an import, so there is nothing to report per-source.
        using ProgressFn = std::function<void(const std::filesystem::path& source, bool ok, const std::string& detail)>;
        void SetProgress(ProgressFn fn);

        // Test-only seam: overrides the importer CookProject calls (default is
        // Arcane::AssetPipeline::ImportTexture) so a test can COUNT invocations across
        // two separate CookSession instances/processes sharing one Intermediate/ tree,
        // instead of inferring the count indirectly. NEVER called by production code --
        // arccook's main.cpp and the future editor caller (Task 12) always get the real
        // importer.
        using ImporterFn = std::function<std::optional<ImportedTexture>(std::span<const std::byte>, const Guid&, const TextureMetaSettings&)>;
        void SetImporterForTesting(ImporterFn fn);

        // Cooks every stale texture source under projectDir/Content, writing artifacts
        // to projectDir/Intermediate/Artifacts. See the file header above for the full
        // staleness/failure/memoization contract. Clears LastFailures() at the start.
        [[nodiscard]] CookResult CookProject(const std::filesystem::path& projectDir);

        // Read-only: true iff at least one texture source under projectDir/Content has
        // no current artifact yet. Never imports, never writes to disk -- the `--check`
        // core, shared verbatim with CookProject's own staleness test. Does not touch
        // LastFailures() (it never imports, so it never has anything to report).
        [[nodiscard]] bool CheckProject(const std::filesystem::path& projectDir) const;

        // Guid -> failure reason from the most recent CookProject call ONLY (cleared at
        // the start of every CookProject call).
        [[nodiscard]] const std::unordered_map<Guid, std::string>& LastFailures() const;

        // Resolves the CURRENT on-disk artifact path for `guid`: finds its source under
        // projectDir/Content, recomputes TODAY's cook key from the source's CURRENT
        // bytes + settings (the exact same key math CookProject/CheckProject use), and
        // returns store.PathFor(that key) IFF a file already exists there. Deliberately
        // NEVER goes through ArtifactStore::RebuildIndexFromScan/Lookup: that index maps
        // one Guid -> ONE cook key by last-write-wins over recursive_directory_iterator
        // order (undefined by contract), so when an orphaned artifact from an earlier
        // `.meta` settings edit still sits on disk under its OLD key -- both it and the
        // current artifact carry the SAME sourceGuid header -- the index can resolve to
        // either one non-deterministically. This function sidesteps that ambiguity
        // entirely by never consulting the index, exactly the same "existence-check on
        // the CURRENT key only" discipline CookProject/CheckProject already use for
        // staleness. Returns nullopt when no source matches `guid`, OR when the source's
        // CURRENT artifact hasn't been cooked yet (uncooked/stale) -- NEVER falls back to
        // any other artifact, orphaned or otherwise. Read-only: never imports, never
        // writes. arccook's --dump-dds is this function's only production caller today.
        [[nodiscard]] std::optional<std::filesystem::path> ResolveCurrentArtifactPath(
            const std::filesystem::path& projectDir, const Guid& guid) const;

    private:
        ProgressFn m_progress;
        ImporterFn m_importer;
        std::unordered_map<Guid, std::string> m_lastFailures;
    };
}
