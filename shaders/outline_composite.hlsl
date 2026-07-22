// Pass 3: distance-field -> anti-aliased two-color outline that STRADDLES the
// silhouette edge (inside + border + outside). Reads the boundary-seeded JFA field
// (nearest silhouette EDGE position + tag) and blends amber/cyan (display-referred)
// over the target. No CPU readback; no sRGB conversion.
//
// The cbuffer/SRV use plain register(b0)/register(t0): the SPIR-V build applies
// -fvk-b-shift 256 0 / -fvk-t-shift 0 0 (matching nvrhi::VulkanBindingOffsets), so
// no #if SPIRV guard is needed. .Load (not .Sample) -- integer texel fetch, no sampler.

cbuffer CompositeCB : register(b0)
{
    float  gSelectThick;   // FULL outline width (px), CENTERED on the silhouette edge
    float  gHoverThick;
    float  gEdgeSoft;      // AA ramp width (px)
    float  _pad0;
    int2   gDim;
    int2   _pad1;
    float4 gSelectColor;   // display-referred (amber)
    float4 gHoverColor;    // display-referred (cyan)
};

Texture2D<float4> gField : register(t0);   // boundary-seeded nearest-EDGE field

struct VSOutput { float4 pos : SV_Position; };

VSOutput vs_main(uint vid : SV_VertexID)
{
    VSOutput o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 ps_main(VSOutput i) : SV_Target0
{
    int2 p = int2(i.pos.xy);

    float4 s = gField.Load(int3(p, 0));
    if (s.w <= 0.0) discard;                       // no silhouette edge reached this pixel

    float2 sp    = (s.xy * 0.5 + 0.5) * float2(gDim);
    float  d     = distance((float2)p + 0.5, sp);  // distance to the nearest silhouette EDGE
    float  halfW = 0.5 * ((s.z >= 0.0) ? gSelectThick : gHoverThick);   // half-width each side
    float  alpha = 1.0 - smoothstep(halfW - gEdgeSoft, halfW, d);       // d unsigned => straddles
    if (alpha <= 0.0) discard;

    float4 col = (s.z >= 0.0) ? gSelectColor : gHoverColor;
    return float4(col.rgb, col.a * alpha);
}
