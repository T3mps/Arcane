// Exterior two-color edge-detect over the R32_UINT hit-proxy id buffer,
// composited (display-referred) over the tonemapped viewport. hoveredId is
// derived IN-SHADER from the cursor pixel -- no CPU readback. Fullscreen
// triangle from SV_VertexID -- no vertex/index buffer.
// Design: docs/superpowers/specs/2026-07-21-arcane-selection-outline-design.md.
//
// The cbuffer uses a plain register(b0): the SPIR-V build applies
// -fvk-b-shift 256 0, matching nvrhi::VulkanBindingOffsets, so no #if SPIRV
// guard is needed (this is a real constant buffer, not a push constant).

cbuffer OutlineCB : register(b0)
{
    uint   gSelectedId;    // 0 = no selection
    int2   gCursorPx;      // viewport-local pixel; x<0 => no hover
    uint   gSelectThick;   // exterior ring radius (px) for the selected outline
    uint   gHoverThick;    // exterior ring radius (px) for the hover outline
    uint3  _pad;
    float4 gSelectColor;   // display-referred (amber)
    float4 gHoverColor;    // display-referred (cyan)
};

Texture2D<uint> gIds : register(t0);

struct VSOutput { float4 pos : SV_Position; };

// Fullscreen triangle from SV_VertexID (no vertex/index buffer).
VSOutput vs_main(uint vid : SV_VertexID)
{
    VSOutput o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Exterior test: pixel `p` is NOT `target` but a texel within `radius` IS.
bool BordersId(int2 p, uint target, int radius)
{
    if (target == 0u) return false;
    if (gIds.Load(int3(p, 0)) == target) return false;   // interior -> not an exterior edge
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
        {
            if (dx == 0 && dy == 0) continue;
            if (gIds.Load(int3(p + int2(dx, dy), 0)) == target) return true;
        }
    return false;
}

float4 ps_main(VSOutput i) : SV_Target0
{
    int2 p = int2(i.pos.xy);
    uint hoveredId = (gCursorPx.x >= 0 && gCursorPx.y >= 0)
                   ? gIds.Load(int3(gCursorPx, 0)) : 0u;

    // Select precedence: the selected outline wins on shared pixels.
    if (BordersId(p, gSelectedId, (int)gSelectThick))
        return gSelectColor;
    if (hoveredId != gSelectedId && BordersId(p, hoveredId, (int)gHoverThick))
        return gHoverColor;
    discard;                 // leave the tonemapped scene untouched
    return float4(0.0, 0.0, 0.0, 0.0);
}
