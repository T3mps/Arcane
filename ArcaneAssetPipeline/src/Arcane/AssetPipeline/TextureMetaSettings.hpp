#pragma once

// Arcane::AssetPipeline::TextureMetaSettings -- the ".meta" "texture" block (import settings).
// STUB for F2b Task 2 (CookKey/ArtifactStore tests only): two fields, using the REAL field
// names the full struct will keep -- Task 3 grows this struct IN PLACE (adds `format` and
// `generateMips`, plus the FromMetaJson/ToMetaJson round-trip and a .cpp) rather than
// replacing it, so ComputeCookKey's explicit-field hashing has real names to hash today.

#include <cstdint>

namespace Arcane::AssetPipeline
{
    struct TextureMetaSettings
    {
        bool srgb = true;
        std::uint32_t maxSize = 0;   // 0 == unlimited
    };
}
