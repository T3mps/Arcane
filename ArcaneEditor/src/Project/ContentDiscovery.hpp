#pragma once

// ContentDiscovery -- F2b desk-checkpoint fix: mid-session content drops are
// discovered. AssetRegistry::ScanContent (the ONLY thing that mints a fresh
// source's ".meta" sidecar and registers a Guid for it) runs exactly once, at
// project open (Project::Open, Project.cpp) -- nothing rescans Content/
// mid-session. So a source file dropped into an already-open project's
// Content/ gets no sidecar, never enters the AssetRegistry, never appears in
// the Assets panel (which builds straight off the registry -- Panels/
// AssetPanelModel.hpp's BuildAssetEntries), and never reaches CookSession's
// own per-kind EnumerateSources either (that function, ArcaneAssetPipeline/
// CookSession.cpp, private, skips any source with no EXISTING ".meta"
// sidecar -- "not a registered source yet").
// The spec's headline ergonomic ("drop a .png -> appears immediately, cooks
// in background") was dead for exactly this mid-session case.
//
// This is the PURE discovery half of the fix -- cheap directory enumeration
// plus a set lookup, no file reads, no hashing, cost proportional to file
// COUNT under Content/, never file size. EditorAppProject.cpp's
// PollAssetWatch calls DiscoverUnknownSources on a slower cadence than its
// own ~1 Hz mtime sweep (see that file's own comment for the cadence and the
// reasoning) and, for anything it returns, calls Runtime::RegisterCreatedAsset
// -- the SAME incremental per-file registration path New Material / New
// Instance / New Mesh already use (Project::RegisterAsset ->
// AssetRegistry::AddFile) -- to mint the sidecar and enter the registry.
// Deliberately NOT a re-run of AssetRegistry::ScanContent: that call CLEARS
// the whole registry first (its own header comment), which would drop every
// plugin/diag:// entry AddContent folded in at open -- the wrong shape for
// "notice one new file".
//
// F2c s4.1, Task 9: generalized from a hardcoded ".png"-only scan
// (EnumerateContentPngFiles/DiscoverUnknownTextureSources) to an arbitrary
// EXTENSION SET, the same widening CookSession's own EnumerateSources went
// through in Task 8 -- a mid-session `.glb` drop has exactly the dead-path
// problem this file's header already described for `.png`, and leaving the
// scan texture-only would make drop-to-cook work for one kind (Texture) and
// silently not the other (Model). The caller (PollAssetWatch) decides WHICH
// extensions to pass; this file stays kind-agnostic, same as
// CookSession::EnumerateSources does for the pipeline side.
//
// Verify/reference PNGs used by capture/bless tooling live OUTSIDE any
// project's Content/ (ReferenceProject/Saved/, golden images, etc.), so they
// are structurally excluded from this scan by construction -- this file
// never needs to know about them.

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Arcane::Editor
{
    // Every regular file under `contentDir` (recursively) whose lowercased
    // extension matches one of `extensions` -- the SAME file-type/recursion
    // rule CookSession::EnumerateSources uses (ArcaneAssetPipeline/
    // CookSession.cpp; private to that TU, so this deliberately mirrors its
    // shape rather than sharing code with it), MINUS that function's "has an
    // existing .meta sidecar" requirement: that rule scopes "cookable right
    // now"; this one scopes "a source that might still need a sidecar
    // minted" -- exactly the sidecar-less state a fresh drop starts in.
    // Sorted for a deterministic return order, matching EnumerateSources' and
    // AssetRegistry::All()'s own determinism rule. Returns empty (never
    // throws) when `contentDir` does not exist. `extensions` is caller-owned
    // and must outlive the call (a std::span, not a copy) -- callers
    // typically pass a `static constexpr std::string_view[]`, same pattern
    // CookSession.cpp's kTextureExtensions/kMeshExtensions already use.
    std::vector<std::filesystem::path> EnumerateContentSourceFiles(
        const std::filesystem::path& contentDir,
        std::span<const std::string_view> extensions);

    // `candidates` (typically EnumerateContentSourceFiles' own return) minus
    // `knownPaths` -- a pure set difference, no filesystem access of its
    // own. Each candidate is compared in generic-string form (forward
    // slashes) so callers building `knownPaths` from paths of either
    // separator style still compare correctly.
    std::vector<std::filesystem::path> UnknownPaths(
        const std::vector<std::filesystem::path>& candidates,
        const std::unordered_set<std::string>& knownPaths);

    // Composition of the two functions above: every source candidate under
    // `contentDir` matching `extensions` that is NOT in `knownPaths`. This is
    // the one function PollAssetWatch actually calls; the two halves above
    // are exposed separately because they are independently useful to test
    // (a real temp-dir enumeration fixture vs. a pure in-memory diff).
    std::vector<std::filesystem::path> DiscoverUnknownSources(
        const std::filesystem::path& contentDir,
        std::span<const std::string_view> extensions,
        const std::unordered_set<std::string>& knownPaths);
}
