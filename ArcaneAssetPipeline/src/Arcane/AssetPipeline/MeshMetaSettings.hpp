#pragma once

// Arcane::AssetPipeline::MeshMetaSettings -- the ".meta" "mesh" block. v1 has NO user-facing
// knobs (spec s5.4, and the comparison's Decision 2 for why that is defensible rather than
// lazy: UE's ImportUniformScale exists as a UNIT CONVERSION, and glTF's unit is already ours --
// there is nothing left to expose). `settingsVersion` is the honest placeholder for that: it
// still hashes into ComputeMeshCookKey, so the day the first real knob lands, the key moves for
// every mesh, exactly once, instead of that first knob silently doing nothing until someone
// remembers to also touch the hash.
//
// FromMetaJson/ToMetaJson round-trip the block through nlohmann::json -- an absent "mesh" block
// (call FromMetaJson with an empty object) or a missing/wrong-typed field within it falls back to
// this struct's own default for that field, never throws. Same tolerant contains()+is_X() gate
// TextureMetaSettings::FromMetaJson uses (TextureMetaSettings.hpp) -- never a bare .value() that
// can throw type_error on a hand-edited file with the wrong JSON type in a field.
//
// BINDING, carried verbatim from TextureMetaSettings.hpp: ComputeMeshCookKey (CookKey.cpp) hashes
// these fields EXPLICITLY, one at a time, in declaration order -- never a struct memcpy (struct
// padding is compiler/ABI dependent, which would make the cook key nondeterministic across
// toolchains). A new field here MUST be added to that hash too, or a settings change silently
// fails to invalidate the cook key and a stale artifact survives it.

#include <cstdint>

#include <Json.hpp>

namespace Arcane::AssetPipeline
{
    struct MeshMetaSettings
    {
        std::uint32_t settingsVersion = 1;

        // Tolerant, defaulted -- reads only fields present with the expected JSON
        // type; anything absent or mistyped keeps this struct's default.
        [[nodiscard]] static MeshMetaSettings FromMetaJson(const nlohmann::json& j);
        [[nodiscard]] nlohmann::json ToMetaJson() const;
    };
}
