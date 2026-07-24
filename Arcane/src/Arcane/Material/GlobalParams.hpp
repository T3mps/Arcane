#pragma once

// GlobalParams: the engine-global shader constants every material sees (UE's
// MaterialParameterCollection, scaled to one register). CPU-side layout only in
// Slice 1 -- Slice 4's FullscreenMaterialPass owns the actual volatile CB, updates
// it once per frame, and binds it at kGlobalCbSlot (b1) next to the material CB at
// kMaterialCbSlot (b0). The struct layout IS the cbuffer layout: one 16-byte
// register, fields in declaration order -- keep it in lockstep with the
// %{MATERIAL_CBUFFER} block the template stitcher emits.

#include <cstdint>

namespace Arcane
{
    // cbuffer register assignments shared by every material template.
    inline constexpr std::uint32_t kMaterialCbSlot = 0;   // cbuffer Material : register(b0)
    inline constexpr std::uint32_t kGlobalCbSlot   = 1;   // cbuffer Globals  : register(b1)

    struct GlobalParams
    {
        float time = 0.0f;             // seconds since app start
        float deltaTime = 0.0f;        // seconds, last frame
        float viewportWidth = 0.0f;    // pixels
        float viewportHeight = 0.0f;   // pixels
    };

    static_assert(sizeof(GlobalParams) == 16, "GlobalParams must stay one cbuffer register");
}
