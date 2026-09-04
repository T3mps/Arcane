#pragma once

// Arcane::AssetPipeline::TextureImporter -- texture importer v1, the RGBA8 path. This lib now
// owns the decode step (stb_image; the implementation TU lives at StbImpl.cpp in this same
// project -- ArcaneClient/src/Arcane/Assets/StbImpl.cpp stays put, the oracle/chrome still
// decode content textures through it).
//
// Pipeline: decode (stb) -> linearize sRGB (when settings.srgb) -> build the mip chain via a
// PLAIN 2x2 box average IN LINEAR FLOAT SPACE, never gamma space (see
// AssetPipelineImporterTest.cpp's canonical correctness case: a 2x2 sRGB image of pure black and
// pure white quads must average to the value whose LINEAR average re-encodes to sRGB ~188, not
// ~128 -- that assertion is what catches an accidental gamma-space average) -> re-encode sRGB on
// the way back to RGBA8 for every level -> build a <=64px-long-edge uncompressed RGBA8 thumbnail
// from the (possibly maxSize-clamped) top level, aspect kept -> return everything
// WriteTextureArtifact (ArtifactFormat.hpp) needs.
//
// Mip dims halve by FLOOR: max(1, dim >> 1) -- 5 -> 2 -> 1, never 3 (a ceil-derived table would
// break Task 7's GPU uploads on the first NPOT texture). Width and height halve independently,
// so an NPOT source stays NPOT down the whole chain. `maxSize` (0 == unlimited) drops the top
// mips whose longer edge exceeds it -- the artifact's own width/height become the clamped size,
// not the original decode size. `generateMips == false` stops after that single (possibly
// clamped) level, so mipCount == 1.
//
// Every Format value (Auto/Bc7/Rgba8) produces an RGBA8 artifact this slice -- bc7enc_rdo is not
// vendored yet. Task 4 replaces the branch that decides this with a real BC7 encode for
// Bc7/Auto; Rgba8 keeps behaving exactly as it does today.

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "Arcane/AssetPipeline/ArtifactFormat.hpp"
#include "Arcane/AssetPipeline/TextureMetaSettings.hpp"
#include "Arcane/Guid.hpp"

namespace Arcane::AssetPipeline
{
    struct ImportedTexture
    {
        TextureArtifactDesc desc;
        std::vector<std::byte> payload;     // every mip's RGBA8 texels, back to back, addressed
                                             // by each MipDesc's own offset/size (mip 0 first)
        std::vector<std::byte> thumbRgba;    // uncompressed RGBA8, desc.thumbWidth x thumbHeight
    };

    // Decodes `pngBytes` (any format stb_image itself supports) and imports it per `settings`.
    // Returns nullopt if the bytes fail to decode as an image -- caller (Task 5's CookSession)
    // turns that into a memoized cook failure, never a crash.
    [[nodiscard]] std::optional<ImportedTexture> ImportTexture(std::span<const std::byte> pngBytes,
                                                                 const Guid& sourceGuid,
                                                                 const TextureMetaSettings& settings);
}
