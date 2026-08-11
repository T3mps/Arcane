#pragma once

// SpriteAsset: the .arcsprite file -- a native JSON asset with an embedded
// top-level "id" (rides AssetRegistry::ScanContent's native path, exactly
// like .arcmat). Standalone asset (not a scene-embedded blob): a sprite
// names a source texture Guid + a pixel sub-rect + a pivot + pixels-per-
// unit, and is meant to be shared by many SpriteRenderer components via
// reference. `sourceSize` of (0,0) means "whole texture" -- authors don't
// have to know texture dimensions up front to make a full-texture sprite.
// ComputeSpriteGeom resolves the pixel rect against the actual texture
// dimensions (known only once the texture asset loads) into UVs + a world
// size in meters, so the renderer never has to re-derive this math itself.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on dll-exported types: benign under /MD
#endif
    struct SpriteAssetData
    {
        Guid        id{};
        std::string name;
        Guid        texture{};                  // source texture asset; nil renders untextured
        float       ppu = 100.0f;               // pixels per meter
        glm::vec2   sourcePos{0.0f, 0.0f};      // sub-rect origin, pixels
        glm::vec2   sourceSize{0.0f, 0.0f};     // sub-rect dims, pixels; (0,0) = whole texture
        glm::vec2   pivot{0.5f, 0.5f};          // normalized
    };

    // Memberwise equality. It exists for the sprite editor's undo bracket,
    // which compares an activation-time COPY of the data against the live data
    // to decide whether a drag actually moved anything before it pushes an
    // undo step (SpriteDocument::PushDataEdit).
    //
    // Memberwise and never memcmp, for two independent reasons: `name` is a
    // std::string, whose object bytes are a pointer/SSO buffer rather than the
    // text; and the trailing scalars (one float plus three vec2 = 28 bytes)
    // land after an 8-byte-aligned prefix (two Guids of two uint64s each, and
    // a std::string), so the struct is padded up to its 8-byte alignment and a
    // byte compare would read that uninitialized tail. Floats compare exactly
    // on purpose -- the question is "did the widget write a different value",
    // not "are these close".
    [[nodiscard]] inline bool operator==(const SpriteAssetData& a, const SpriteAssetData& b) noexcept
    {
        return a.id == b.id && a.name == b.name && a.texture == b.texture &&
               a.ppu == b.ppu && a.sourcePos == b.sourcePos &&
               a.sourceSize == b.sourceSize && a.pivot == b.pivot;
    }

    // Write `data` as .arcsprite JSON. Only non-default rect/pivot fields are
    // written, so a plain full-texture sprite stays a minimal file (absent
    // keys resolve to the SpriteAssetData defaults on load). False on IO
    // failure.
    ARCANE_API bool SaveSpriteAsset(const std::filesystem::path& path, const SpriteAssetData& data);

    // Parse a .arcsprite. nullopt on IO/parse failure or when the file is not
    // a sprite asset (no structurally-unique key distinguishes a sprite, so
    // the "type":"sprite" tag IS the discriminator). Malformed individual
    // fields fall back to their SpriteAssetData default rather than failing
    // the whole load.
    ARCANE_API std::optional<SpriteAssetData> LoadSpriteAsset(const std::filesystem::path& path);

    // The pixel sub-rect + pivot resolved against a texture's actual pixel
    // dimensions: normalized UVs for sampling, and the sprite's world-space
    // size in meters (size-in-pixels / ppu).
    struct ResolvedSpriteGeom
    {
        glm::vec2 uvMin;
        glm::vec2 uvMax;
        glm::vec2 sizeMeters;
    };

    // texWidth/texHeight are the source texture's actual pixel dimensions
    // (0 before the texture asset has loaded -- returns a safe 1x1 m,
    // full-UV fallback rather than dividing by zero).
    ARCANE_API ResolvedSpriteGeom ComputeSpriteGeom(const SpriteAssetData& data,
                                                     std::uint32_t texWidth, std::uint32_t texHeight);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
