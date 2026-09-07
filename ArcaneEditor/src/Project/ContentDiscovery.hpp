#pragma once

// ContentDiscovery -- F2b desk-checkpoint fix: mid-session texture drops are
// discovered. AssetRegistry::ScanContent (the ONLY thing that mints a fresh
// PNG's ".meta" sidecar and registers a Guid for it) runs exactly once, at
// project open (Project::Open, Project.cpp) -- nothing rescans Content/
// mid-session. So a .png dropped into an already-open project's Content/
// gets no sidecar, never enters the AssetRegistry, never appears in the
// Assets panel (which builds straight off the registry -- Panels/
// AssetPanelModel.hpp's BuildAssetEntries), and never reaches CookSession::
// EnumerateTextureSources either (that function, ArcaneAssetPipeline/
// CookSession.cpp, private, skips any .png with no EXISTING ".meta" sidecar
// -- "not a registered source yet").
// The spec's headline ergonomic ("drop a .png -> appears immediately, cooks
// in background") was dead for exactly this mid-session case.
//
// This is the PURE discovery half of the fix -- cheap directory enumeration
// plus a set lookup, no file reads, no hashing, cost proportional to file
// COUNT under Content/, never file size. EditorAppProject.cpp's
// PollAssetWatch calls DiscoverUnknownTextureSources on a slower cadence
// than its own ~1 Hz mtime sweep (see that file's own comment for the
// cadence and the reasoning) and, for anything it returns, calls
// Runtime::RegisterCreatedAsset -- the SAME incremental per-file
// registration path New Material / New Instance / New Mesh already use
// (Project::RegisterAsset -> AssetRegistry::AddFile) -- to mint the sidecar
// and enter the registry. Deliberately NOT a re-run of
// AssetRegistry::ScanContent: that call CLEARS the whole registry first
// (its own header comment), which would drop every plugin/diag:// entry
// AddContent folded in at open -- the wrong shape for "notice one new file".
//
// Verify/reference PNGs used by capture/bless tooling live OUTSIDE any
// project's Content/ (ReferenceProject/Saved/, golden images, etc.), so they
// are structurally excluded from this scan by construction -- this file
// never needs to know about them.

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace Arcane::Editor
{
    // Every ".png" (case-insensitive extension) regular file under
    // `contentDir`, recursively -- the SAME file-type/recursion rule
    // CookSession::EnumerateTextureSources uses (ArcaneAssetPipeline/
    // CookSession.cpp; private to that TU, so this deliberately mirrors its
    // shape rather than sharing code with it), MINUS that function's "has an
    // existing .meta sidecar" requirement: that rule scopes "cookable right
    // now"; this one scopes "a texture source that might still need a
    // sidecar minted" -- exactly the sidecar-less state a fresh drop starts
    // in. Sorted for a deterministic return order, matching
    // EnumerateTextureSources' and AssetRegistry::All()'s own determinism
    // rule. Returns empty (never throws) when `contentDir` does not exist.
    std::vector<std::filesystem::path> EnumerateContentPngFiles(const std::filesystem::path& contentDir);

    // `candidates` (typically EnumerateContentPngFiles' own return) minus
    // `knownPaths` -- a pure set difference, no filesystem access of its
    // own. Each candidate is compared in generic-string form (forward
    // slashes) so callers building `knownPaths` from paths of either
    // separator style still compare correctly.
    std::vector<std::filesystem::path> UnknownPaths(
        const std::vector<std::filesystem::path>& candidates,
        const std::unordered_set<std::string>& knownPaths);

    // Composition of the two functions above: every texture-source
    // candidate under `contentDir` that is NOT in `knownPaths`. This is the
    // one function PollAssetWatch actually calls; the two halves above are
    // exposed separately because they are independently useful to test (a
    // real temp-dir enumeration fixture vs. a pure in-memory diff).
    std::vector<std::filesystem::path> DiscoverUnknownTextureSources(
        const std::filesystem::path& contentDir,
        const std::unordered_set<std::string>& knownPaths);
}
