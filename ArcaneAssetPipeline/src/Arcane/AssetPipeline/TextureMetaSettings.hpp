#pragma once

// Arcane::AssetPipeline::TextureMetaSettings -- the ".meta" "texture" block (import settings).
// Grown in place for F2b Task 3 from the Task 2 two-field stub (`srgb`, `maxSize`) to its full
// shape: `format` and `generateMips` are new; the two original fields keep their names, so
// ComputeCookKey's existing hashing of them stays valid. FromMetaJson/ToMetaJson round-trip the
// block through nlohmann::json -- an absent "texture" block (call FromMetaJson with an empty
// object) or a missing/wrong-typed field within it falls back to this struct's own default for
// that field, never throws. Same tolerant contains()+is_X() gate Arcane::Diag::QueueFromJson
// uses (DiagEnvelope.cpp) -- never a bare .value() that can throw type_error on a hand-edited
// file with the wrong JSON type in a field.
//
// BINDING for future field additions: CookKey.cpp's ComputeCookKey hashes these fields
// EXPLICITLY, one at a time, in declaration order -- it never memcpy's this struct (struct
// padding is compiler/ABI dependent, which would make the cook key nondeterministic across
// toolchains). A new field here MUST be added to that hash too, or a settings change silently
// fails to invalidate the cook key and a stale artifact survives a settings edit.

#include <cstdint>

#include <Json.hpp>

namespace Arcane::AssetPipeline
{
    struct TextureMetaSettings
    {
        enum class Format : std::uint8_t { Auto, Bc7, Rgba8 };

        Format format = Format::Auto;   // Auto == Bc7 this slice (BC7 encode arrives Task 4;
                                         // ImportTexture treats every Format as RGBA8 until then)
        bool srgb = true;
        bool generateMips = true;
        std::uint32_t maxSize = 0;   // 0 == unlimited

        // Tolerant, defaulted: reads only the fields present with the expected JSON type;
        // anything absent or mistyped keeps this struct's default for that field.
        [[nodiscard]] static TextureMetaSettings FromMetaJson(const nlohmann::json& j);
        [[nodiscard]] nlohmann::json ToMetaJson() const;
    };
}
