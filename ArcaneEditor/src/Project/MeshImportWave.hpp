#pragma once

// MeshImportWave -- the editor's PURE helpers for the mesh import wave (F2c Task 13,
// spec s5.5/s6, A4). PURE by design (no ImGui, no Project, no device) so ArcaneTests
// drives them directly -- the AssetPanelModel/CookQueue/ContentDiscovery pattern.
// REMINDER: this .cpp must be added to ArcaneTests' EXPLICIT editor TU list in the
// root premake5.lua (beside ContentDiscovery.cpp), or the test will not link.
//
// A .glb's embedded images have no registered source, no sidecar and no cook key of
// their own, so they cannot ride the ordinary texture-cook path at all -- extraction
// to a loose .png SIBLING (which the ordinary path then picks up on its own next
// discovery sweep) is what makes them cookable. ExtractEmbeddedTextures is the whole
// of that: SurveyGltf (ArcaneAssetPipeline/GltfSurvey.hpp) does the read-only glTF
// parse, this file decides WHERE each embedded image lands on disk and enforces A4's
// never-overwrite rule before writing.

#include <Arcane/Mesh/MeshAsset.hpp>   // MeshSlot; transitively LoadedClientMesh/MeshSectionView
                                        // (ArtifactReader.hpp) -- Task 14's slot reconciliation
#include <Arcane/Mesh/MeshBuilder.hpp>   // Arcane::MeshBounds -- F2c Plan 2 Task 9's FrameMeshBounds

#include <glm/glm.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    // `dir`/`stem``ext`, suffixed "-1", "-2", ... until the name is free on disk.
    // A4's collision rule, shared by extracted textures, minted companions and minted
    // materials -- one definition, because three near-copies is how two of them drift.
    [[nodiscard]] std::filesystem::path UniqueSiblingPath(
        const std::filesystem::path& dir, const std::string& stem, const std::string& ext);

    // A glTF image's file stem: its own name when it has one AND that name is
    // "usable" once sanitised, else "<sourceStem>-<index>" (A4's source-derived
    // fallback). Sanitised to filesystem-safe characters, because a glTF name is
    // arbitrary UTF-8 and a '/' in one would silently write outside the intended
    // folder. "Usable" is its own gate on top of that: a name that sanitises down to
    // empty, or to nothing but dots (a bare "." or ".." -- inert against escaping the
    // folder once separators are gone, but still not a name a file can be created
    // under), also falls back to the source-derived name -- treated as "no usable
    // name" exactly like an empty glTF-authored name is.
    [[nodiscard]] std::string ImageFileStem(const std::string& imageName,
                                            const std::string& sourceStem, std::size_t index);

    // The loose sibling's extension, implied by a glTF image's declared MIME type
    // ("image/png" -> ".png", "image/jpeg"/"image/jpg" -> ".jpg", anything else this
    // engine has not been handed a fixture for falls back to ".png" rather than
    // growing an untested branch). PUBLIC (Task 13 kept this file-local) because Task
    // 15's material mint (EditorAppProject.cpp) needs the SAME mapping to re-derive
    // which extension an embedded image was extracted under -- one definition, so the
    // write side (ExtractEmbeddedTextures) and the read side (FindExtractedImagePath
    // below) can never drift.
    [[nodiscard]] std::string ExtensionForMime(const std::string& mimeType);

    // A newly-discovered `.gltf`/`.glb`'s embedded images, extracted to loose .png
    // siblings beside `source`. SurveyGltf's front half first (nullopt -- the file
    // will not cook either -- means no extraction, quietly); then, per EMBEDDED image
    // in survey order, a destination is resolved from `source.parent_path()` /
    // (ImageFileStem's stem + the extension `mimeType` implies, "image/png" ->
    // ".png" etc.) by walking a CHAIN of candidates.
    //
    // Spec s5.5 states two invariants that both have to hold at once: an already-
    // extracted .png is NEVER overwritten, and (A4) a name COLLISION is suffixed
    // rather than silently reused. A plain "skip if the destination exists" honors
    // only the first -- it would hand two different sources embedding a same-named
    // image the SAME texture, exactly what A4 exists to prevent. The reconciliation
    // walks the SAME candidate chain UniqueSiblingPath itself walks -- the natural
    // name, then `-1`, `-2`, ... -- applying ONE byte-compare rule at every EXISTING
    // candidate, with no separate "natural" case and "collision" case:
    //   - a candidate that does not exist yet   -> write there. This is reached
    //     immediately (the natural name) when nothing has ever collided, or after
    //     skipping past one or more EXISTING-BUT-DIFFERENT candidates otherwise --
    //     both are the same rule, just reached at a different point in the walk.
    //   - an EXISTING candidate, IDENTICAL bytes -> already extracted right HERE (a
    //     harmless duplicate, or this exact re-extraction already ran) -> the walk
    //     stops, nothing is written.
    //   - an EXISTING candidate, DIFFERENT bytes -> not a match (a user's edit,
    //     another source's own image, or an EARLIER divergence's own sibling) ->
    //     never overwritten; the walk continues to the next suffix.
    // The chain walk (rather than checking the natural name alone) is what keeps a
    // REPEATED divergence from piling up a fresh numbered duplicate on every
    // re-extraction: once a diverged natural name has already produced `stem-1`,
    // re-extracting again matches `stem-1`'s own bytes and stops there, rather than
    // skipping past it (it differs from the still-diverged natural name, which it is
    // never compared against) straight into minting `stem-2`. Re-discovering an
    // entirely unmodified extraction is likewise a true no-op (matches at the natural
    // name itself) -- the pile only grows for an actual NEW divergence, which is the
    // rare case by construction (first discovery, or a genuine edit/collision).
    //
    // Returns only the paths this call actually WROTE, which is what makes a second,
    // all-matched call's empty return the proof that nothing changed.
    [[nodiscard]] std::vector<std::filesystem::path> ExtractEmbeddedTextures(
        const std::filesystem::path& source);

    // The READ-ONLY half of the SAME chain walk ExtractEmbeddedTextures runs above
    // (this header's own comment on that function has the full account) -- never
    // writes. Task 15 (material mint, spec s6 step 5) needs the EXACT path an
    // embedded image's bytes were already extracted to, which is not re-derivable
    // from the name alone once a collision has occurred (identical bytes -> the
    // existing file; different bytes -> a suffixed sibling): re-running this SAME
    // logic read-only, rather than duplicating the chain, is what keeps the two in
    // lockstep.
    //
    // Walks `dir`/`stem``ext`, `dir`/`stem`-1`ext`, ... and returns the first
    // EXISTING candidate whose on-disk bytes equal `bytes` (the one
    // ExtractEmbeddedTextures itself would have stopped at); nullopt when no such
    // candidate exists -- the image has not been extracted under this chain at all
    // (not yet discovered, or the discovery sweep has not run since).
    [[nodiscard]] std::optional<std::filesystem::path> FindExtractedImagePath(
        const std::filesystem::path& dir, const std::string& stem, const std::string& ext,
        const std::vector<std::byte>& bytes);

    // ---- F2c Task 14 (spec s4.2, R3): the companion .arcmesh mint + slot
    // reconciliation on re-cook -------------------------------------------------------

    // The editor-side mirror of AssetPipeline::SlotNamesFromSections (ArtifactFormat.hpp):
    // the SAME two-line derivation (max slotIndex -> array size; the FIRST section
    // touching a slot names it, and A1's dedup-by-name guarantees every later section
    // pointing at that slot agrees) -- but over Arcane::MeshSectionView rather than
    // AssetPipeline::MeshArtifactSection. The two structs mirror the SAME on-disk shape
    // BY HAND, never by sharing code (ArtifactReader.hpp's own BYTE-CONTRACT PEER banner),
    // so the pipeline's own SlotNamesFromSections is not reachable from here -- the
    // artifact the editor's companion mint actually holds is a LoadedClientMesh
    // (Assets::MeshArtifactFor's return type), whose `.sections` are MeshSectionView, not
    // MeshArtifactSection. Sized to max(slotIndex)+1, empty when `sections` is empty.
    [[nodiscard]] std::vector<std::string> SlotNamesFromSections(
        const std::vector<Arcane::MeshSectionView>& sections);

    // MeshImportWave.hpp -- R3's reconciliation, PURE so it is testable without a
    // project, a registry or a cook.
    struct SlotReconciliation
    {
        std::vector<Arcane::MeshSlot> slots;      // the new slot array to write
        std::vector<std::string>      warnings;   // one per vanished-but-kept name
    };

    // `existing` is the companion's current slots; `authoritative` is the freshly
    // cooked artifact's slot names (SlotNamesFromSections' answer, in slot order).
    //
    // THE RULES, from R3 / s4.2. Three are UE's, read rather than inferred; the
    // fourth is OURS and is labelled as such -- do not re-attribute it.
    //   * match by NAME -- an existing slot whose name appears in `authoritative`
    //     keeps its material assignment, wherever it moved to
    //     (FbxStaticMeshImport.cpp:1964-1970: UE compares ImportedMaterialSlotName and
    //     BREAKS on the first match);
    //   * APPEND unmatched authoritative names, in authoritative order
    //     (the append-if-unmatched arm at FbxStaticMeshImport.cpp:1964-1974);
    //   * NEVER DELETE -- an existing slot whose name vanished is KEPT and WARNED.
    //     Deleting a user's material assignment over a re-export hiccup is worse than
    //     carrying a harmless orphan; UE keeps it SILENTLY (there is no deletion arm),
    //     and keep-and-WARN is a strict improvement on that (the comparison's
    //     Decision 3 says so in as many words);
    //   * POSITION is the tiebreak for unnamed slots and duplicate names -- the case
    //     A1's by-name dedup makes rare but cannot make impossible (two DISTINCT glTF
    //     materials may both be unnamed). UE's analogous positional fallback -- the one
    //     that resolves a SECTION against the FINAL slot array -- is at
    //     FbxStaticMeshImport.cpp:1991-2002, NOT at :1946-1974 (that block's :1948
    //     falls back into the NEWLY IMPORTED array, a different array and a different
    //     question). Cite :1991-2002 for this rule.
    //
    // OURS, NOT UE'S -- the CONSUME-MATCHED rule (Step 3): once an existing slot has
    // been matched, it is removed from the candidate pool so a later authoritative
    // name with the same spelling cannot match it again. UE does NOT do this -- its
    // matcher breaks on the first match and never consumes it (:1964-1970), so two
    // identically-named candidates would both bind to the same existing slot there.
    // We consume because the two-unnamed-slots case REQUIRES it: without consumption
    // both unnamed authoritative names match existing slot 0 and the second user
    // assignment is silently lost. This is a deliberate divergence, and the
    // "unnamed and duplicate names fall back to POSITION" test is what pins it.
    [[nodiscard]] SlotReconciliation ReconcileSlots(
        const std::vector<Arcane::MeshSlot>& existing,
        const std::vector<std::string>& authoritative);

    // ---- F2c Task 15 (spec s6, R4): material minting -- reuse-by-name, else mint an
    // INSTANCE of the shared import base ------------------------------------------

    // MeshImportWave.hpp -- the pure half of R4's step (1). Given the registry's
    // material assets (guid + mount path + surface) and a glTF material name, which
    // existing asset should the slot point at?
    //
    // R4's rule, and the shape UE takes (FbxImportUI.h:188-194, FbxMaterialImport.cpp:
    // 571/624-689): reuse an existing MESH-surface material whose STEM equals the glTF
    // material name. EXACTLY ONE match reuses; zero or several mint fresh -- the same
    // never-guess-among-duplicates rule MintOrReuseSpriteForTexture already keeps, and
    // for the same reason: picking one of two identically-named materials would be a
    // coin flip the user cannot see.
    struct MaterialCandidate { Arcane::Guid guid; std::string stem; bool meshSurface = false; };

    [[nodiscard]] Arcane::Guid FindReusableMeshMaterial(
        std::span<const MaterialCandidate> candidates, const std::string& gltfMaterialName);

    // F2c Plan 2 Task 6: among (meshGuid, importedSource) pairs, the unique
    // companion of `modelGuid`. Zero or several matches -> nullopt (never guess
    // -- the same rule MintOrUpdateCompanionMesh uses). Cook completion
    // invalidates THIS guid, never the model guid.
    [[nodiscard]] inline std::optional<Arcane::Guid> UniqueImportedCompanion(
        const Arcane::Guid& modelGuid,
        std::span<const std::pair<Arcane::Guid, Arcane::Guid>> meshIdAndImportedSource)
    {
        std::optional<Arcane::Guid> found;
        int n = 0;
        for (const auto& [meshId, imported] : meshIdAndImportedSource)
        {
            if (imported == modelGuid)
            {
                ++n;
                found = meshId;
            }
        }
        return n == 1 ? found : std::nullopt;
    }

    // ---- F2c Plan 2 Task 9 (spec s8, R5): mesh-thumbnail framing --------------------

    // The framing math, pulled out PURE so it is testable without a device -- the same
    // split MeshResidencyBudget takes from NriMeshBufferCache, and for the same reason.
    // Given a local-space AABB and a vertical FOV, where does the camera sit to frame
    // the whole box with a small margin, looking at its centre?
    //
    // MARGIN, not a tight fit: a box that exactly fills the frame reads as cropped at
    // thumbnail size, and the browser draws these at 64px. 15% is the mocks' own feel.
    //
    // A DEGENERATE BOX (a zero-extent AABB -- ComputeMeshBounds' documented answer for
    // an empty mesh) yields a finite camera at a unit distance rather than a division
    // by zero. An empty mesh has nothing to frame, and the harvest will produce an
    // empty picture, which is the honest result -- but it must not produce a NaN
    // transform, which is undefined behaviour on the GPU rather than a blank image.
    struct MeshThumbCamera { glm::vec3 eye; glm::vec3 target; float nearZ; float farZ; };
    [[nodiscard]] MeshThumbCamera FrameMeshBounds(const Arcane::MeshBounds& bounds,
                                                  float fovDegrees);
}
