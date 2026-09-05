#pragma once

// Arcane::ArtifactReader -- the ArcaneClient-local reader for ".arcart" texture artifacts.
//
// BYTE-CONTRACT PEER: ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactFormat.hpp/.cpp
// (Task 1's writer) is the OTHER half of this contract. This file is a DELIBERATE,
// INDEPENDENT reimplementation -- ArcaneClient must never link or #include
// ArcaneAssetPipeline (premake keeps that structurally true: ArcaneClient's includedirs
// carry no AssetPipeline path), so the two sides stay byte-compatible ONLY by both
// following the SAME written format, never by sharing code. A change to the on-disk
// layout in ArtifactFormat.cpp must be mirrored here BY HAND, or the cross-lib round-trip
// test (ArtifactReaderTest.cpp, which writes fixtures through the pipeline's own
// WriteTextureArtifact and reads them back through THIS reader) fails loudly. Add/keep
// this same paragraph, naming this file, in ArtifactFormat.hpp's own header comment.
//
// ON-DISK FORMAT (mirrors ArtifactFormat.hpp's header comment field-for-field; every
// multi-byte field little-endian, one byte at a time -- never a struct memcpy, for the
// same reason ArtifactFormat.cpp's own writer states: struct padding is compiler/ABI
// dependent):
//   magic "ARCA" (4 bytes)
//   artifactVersion   u32  (== 1)
//   contentKind       u8   (1 == Texture)
//   sourceGuid.hi     u64
//   sourceGuid.lo     u64
//   sourceHash        u64  (FNV-1a 64 over the RAW SOURCE bytes -- see HashSourceBytes
//                            below; TextureImporter.cpp computes the SAME fingerprint
//                            when it writes an artifact's header)
//   importerVersion   u32
//   format            u8   (ArtifactPixelFormatValue below mirrors this byte's meaning)
//   dimension         u8
//   arrayOrDepth      u32
//   width             u32
//   height            u32
//   mipCount          u32
//   srgb              u8   (0/1)
//   thumbWidth        u32
//   thumbHeight       u32
//   sectionCount      u32
//   sectionCount * { tag u32, offset u64, size u64 }   -- the section table
//   section bodies, at their own table offsets:
//     MipTable  (tag 1): mipCount u32, then {offset u64, size u64, width u32, height u32}
//                        per mip, in order (slice-major, mip-minor when arrayOrDepth > 1)
//     Payload   (tag 2): raw texel bytes for every mip, back to back, addressed by each
//                        mip's own offset/size into this section
//     Thumbnail (tag 3): raw, uncompressed RGBA8 bytes, thumbWidth x thumbHeight
//   A section tag this reader does not recognise is SKIPPED, not an error -- the same
//   forward-compat rule ArtifactFormat.hpp's reader states (a future artifact kind, e.g.
//   F2c's mesh artifacts, can add a section without breaking this reader).
//
// GUID -> ARTIFACT RESOLUTION (this task's own judgment call): there is no index FILE on
// disk to read -- ArtifactStore's Guid -> cook-key index (ArtifactStore.hpp) is IN-MEMORY
// ONLY, rebuilt by scanning Artifacts/**/*.arcart; nothing in ArcaneAssetPipeline or
// arccook ever persists it. FindArtifactForGuid below therefore does its OWN directory
// scan of <intermediateDir>/Artifacts/**/*.arcart, reading only a BOUNDED PREFIX of each
// candidate (ArtifactReader.cpp's ReadFilePrefix/kHeaderProbeBytes -- 256 bytes, generous
// headroom over the fixed header's own exact 64) to test the sourceGuid match -- a few
// hundred bytes per candidate file, never the whole artifact (payload/thumbnail
// included), even across a project's whole artifact set, and it needs no persistent state
// of its own. CORRECTNESS NOTE: this paragraph used to claim exactly this cost while the
// code underneath (ReadHeaderOnly -> ReadWholeFile) actually read every candidate's ENTIRE
// file every scan -- caught by review, fixed the same task the claim was made in
// (ArtifactReader.cpp's ReadHeaderOnly/ReadFilePrefix carry the fix's own comment); take
// this kind of claim as something to VERIFY against the code, not trust from a comment.
//
// C1(b) FIX (final-review wave, 2026-09-04): FindArtifactForGuid returns EVERY
// guid-matching candidate, not just the first the scan happens to visit. The single-match
// version was a real bug: a recook under a NEW cook key can leave the SUPERSEDED old-key
// artifact still on disk for one pass (CookSession's own self-heal, C1a, removes it
// best-effort, but a locked file or an external tool can still leave one behind), and
// BOTH the stale and the fresh artifact carry the SAME sourceGuid header -- so a
// first-match resolve could pick the stale one in unspecified directory-iteration order
// and refuse HashMismatch forever even though a valid artifact sits right next to it. The
// caller (Assets.cpp's ResolveArtifact) is the one that actually VALIDATES each candidate
// via ReadClientArtifact -- this function stays a cheap, header-only, format-agnostic
// scan; it does not itself decide which candidate wins. See Assets.cpp for how the result
// is memoized per-Guid at the facade layer (the SAME decode-once-then-cache shape
// PixelsFor already uses), which is what keeps a repeat lookup for the same guid from
// re-scanning (Missing included -- Task 8 memoized it alongside the other refusals;
// Task 12's cook-completion invalidation is the un-latch).
//
// REFUSAL DISCIPLINE (spec s5, F2b Task 6 ruling; Task 8 completed it -- "refuse, never limp"):
//   Missing                 -- no artifact at all resolves for this guid. A REAL refusal
//                               at the Assets facade layer since Task 8 (the sprite
//                               cutover): content is artifact-only, the stb fallback is
//                               retired, and a missing artifact refuses by name
//                               ("ArtifactMissing"), memoized and latched like the other
//                               two states below.
//   HashMismatch             -- an artifact EXISTS for this guid, but its header sourceHash
//                               disagrees with the hash of the CURRENT staged source bytes:
//                               the cook is stale (a source was edited without recooking)
//                               or broken. Refuses loudly -- serving stale pixels over a
//                               known-bad artifact would mask exactly the failure the cook
//                               step exists to catch.
//   VersionNewerThanEngine   -- the artifact's importerVersion is NEWER than this build's
//                               own kClientTextureImporterVersionMirror: THIS ENGINE is the
//                               stale side (a downgrade, or a build-skew cook farm). Refuses
//                               loudly for the same reason.
//   SUBSUMPTION CLAUSE (restated from the spec): an artifact OLDER than the engine
//   (importerVersion < the mirror) is NOT a third refusal kind -- it collapses into Missing.
//   CookSession's own staleness check folds importerVersion into the cook KEY
//   (ComputeCookKey), so an artifact cooked under an older importer version already sits at
//   a DIFFERENT key than what today's importer would produce; the CURRENT key simply has no
//   artifact there, which the ordinary Missing path already covers without a distinct case.
//   Only a NEWER importerVersion is genuinely a different situation (this engine cannot
//   even interpret the format that artifact might be using), which is why it alone earns a
//   named refusal.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace Arcane
{
    // Client-local MIRROR of Arcane::AssetPipeline::ArtifactPixelFormat's VALUES
    // (ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactFormat.hpp) -- deliberately
    // NOT shared (see this file's header banner). The numeric values below must stay in
    // lockstep with that enum BY HAND: RGBA8=0, BC7=1, BC5_Reserved=2, BC6H_Reserved=3.
    // A drift here is caught immediately by ArtifactReaderTest.cpp's cross-lib
    // byte-contract case: it writes through the pipeline's REAL enum and reads the byte
    // back through this one, so a renumber on either side that the other did not mirror
    // shows up as a wrong `format` in the very first round-trip assertion.
    enum class ArtifactPixelFormatValue : std::uint8_t
    {
        RGBA8        = 0,
        BC7          = 1,
        BC5_Reserved = 2,
        BC6H_Reserved = 3,
    };

    // Mirrors ArtifactFormat.hpp's MipDesc (minus the pipeline type): offset/size are a
    // VIEW into LoadedClientArtifact::payload below, width/height are this mip's own true
    // (unpadded -- see TextureImporter.cpp's own BC7 padding note) dimensions.
    struct MipView
    {
        std::uint64_t offset = 0;
        std::uint64_t size   = 0;
        std::uint32_t width  = 0;
        std::uint32_t height = 0;
    };

    // Header dims -- ALWAYS the artifact's own authoritative (possibly maxSize-clamped;
    // see TextureImporter.cpp) TOP-LEVEL dims, independent of the thumbnail's own (always
    // <=64px) dims. This is Assets::TextureInfoFor's payload (Assets.hpp) -- the dims a
    // sprite's geometry should use, never the thumbnail's.
    struct TextureInfo
    {
        std::uint32_t width    = 0;
        std::uint32_t height   = 0;
        std::uint32_t mipCount = 0;
        bool          srgb     = false;
    };

    // Task 7 consumes this EXACT name/shape (F2b Task 6 brief) -- do not rename or reshape
    // a member without checking Task 7's consumer first.
    struct LoadedClientArtifact
    {
        TextureInfo                 info;
        ArtifactPixelFormatValue    format = ArtifactPixelFormatValue::RGBA8;
        std::vector<MipView>        mips;
        std::vector<std::byte>      payload;     // every mip's encoded texels, back to back
        std::vector<std::byte>      thumbRgba;   // uncompressed RGBA8, thumbWidth x thumbHeight
        std::uint32_t               thumbWidth  = 0;
        std::uint32_t               thumbHeight = 0;
    };

    // The refusal states a caller must handle per this file's header banner above.
    enum class ArtifactRefusal
    {
        None,
        Missing,
        HashMismatch,
        VersionNewerThanEngine,
    };

    struct ArtifactReadResult
    {
        ArtifactRefusal                       refusal = ArtifactRefusal::Missing;
        std::optional<LoadedClientArtifact>   artifact;   // set iff refusal == None
    };

    // This engine's own copy of the pipeline's kTextureImporterVersion
    // (ArcaneAssetPipeline/src/Arcane/AssetPipeline/CookKey.hpp) -- mirrored BY HAND,
    // never included, per this file's no-shared-code banner above. Bump this number IN
    // LOCKSTEP with that one; a mismatch left standing would make every artifact THIS
    // engine's own cook produces look "newer than the engine" to itself the moment the
    // pipeline-side constant moves without a matching edit here.
    inline constexpr std::uint32_t kClientTextureImporterVersionMirror = 1;

    // Reads and validates ONE .arcart file at `path` against the CURRENT staged source
    // bytes (`currentSourceBytes` -- e.g. a fresh read of the .png this guid resolves to)
    // and `expectedSourceGuid`. `ArtifactRefusal::Missing` covers every "this artifact
    // cannot be used" case that is not one of the two named refusals: the file does not
    // exist, fails to parse (bad magic / truncated / corrupt section table -- the exact
    // conditions ArtifactFormat.hpp's own ReadTextureArtifact treats as nullopt), or its
    // header sourceGuid does not match `expectedSourceGuid` (a stray or renamed artifact
    // file sitting where the caller expected a different one).
    [[nodiscard]] ARCANE_API ArtifactReadResult ReadClientArtifact(
        const std::filesystem::path& path,
        std::span<const std::byte> currentSourceBytes,
        const Guid& expectedSourceGuid);

    // Guid -> EVERY matching artifact path via a DIRECTORY SCAN of
    // intermediateDir/Artifacts/**/*.arcart (this task's resolution choice -- see this
    // file's header banner for why no index file exists to read instead, and its C1(b)
    // paragraph for why this returns ALL matches rather than the first). Empty when no
    // artifact under the store carries this guid -- NOT itself a refusal; see
    // ArtifactRefusal::Missing's own doc above for what IS a refusal. Ordinarily a single
    // element (the common case: exactly one artifact per guid); more than one means a
    // stale, superseded artifact is still on disk alongside the current one (C1a/C1b) --
    // the caller (Assets.cpp's ResolveArtifact) is responsible for validating candidates
    // and picking the first clean one, never this function.
    [[nodiscard]] ARCANE_API std::vector<std::filesystem::path> FindArtifactForGuid(
        const std::filesystem::path& intermediateDir, const Guid& guid);
}
