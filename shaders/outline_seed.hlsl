// Pass 1 of the JFA selection outline: read the SUPERSAMPLED (ss x) R32_UINT id
// buffer and write an RGBA16_SNORM seed { sub-pixel silhouette EDGE pos (normalized),
// tag, coverage } at 1x (composite) resolution -- but ONLY at pixels on the BOUNDARY
// of the selected/hovered silhouette. Fully-interior pixels write an empty seed and
// are filled by the flood, so the field encodes distance-to-EDGE on BOTH sides and
// the composite can straddle the silhouette (inside + border + outside). hoveredId is
// the id at the cursor texel, in-shader.
// Design: docs/superpowers/specs/2026-07-21-arcane-selection-outline-jfa-design.md.

cbuffer SeedCB : register(b0)
{
    uint  gSelectedId;   // 0 = no selection
    int2  gCursorPx;     // 1x viewport px; x<0 => no hover
    uint  gSuperSample;  // id-buffer supersample factor (e.g. 2)
    int2  gDim;          // 1x (composite) dimensions
    uint2 _pad;
};

Texture2D<uint> gIds : register(t0);   // supersampled id buffer (ss*gDim)

struct VSOutput { float4 pos : SV_Position; };

VSOutput vs_main(uint vid : SV_VertexID)
{
    VSOutput o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// .xy = normalized 1x position [-1,1], .z = tag (+1 select, -1 hover),
// .w = coverage (0..1; 0 => empty seed: background OR silhouette interior).
float4 ps_main(VSOutput i) : SV_Target0
{
    int2 p  = int2(i.pos.xy);
    int  ss = (int)gSuperSample;

    uint hoveredId = 0u;
    if (gCursorPx.x >= 0 && gCursorPx.y >= 0)
    {
        int2 c = gCursorPx * ss + (ss / 2);
        hoveredId = gIds.Load(int3(c, 0));
        if (hoveredId == gSelectedId) hoveredId = 0u;   // hovering the selection => amber only
    }

    // Coverage + sub-pixel centroid of each silhouette within THIS 1x pixel.
    int    nSel = 0, nHov = 0;
    float2 sumSel = float2(0, 0), sumHov = float2(0, 0);
    int2   base = p * ss;
    [loop] for (int sy = 0; sy < ss; ++sy)
    [loop] for (int sx = 0; sx < ss; ++sx)
    {
        uint   id  = gIds.Load(int3(base + int2(sx, sy), 0));
        float2 sub = (float2(base + int2(sx, sy)) + 0.5) / (float)ss;   // 1x-space subsample center
        if (gSelectedId != 0u && id == gSelectedId)      { nSel++; sumSel += sub; }
        else if (hoveredId != 0u && id == hoveredId)     { nHov++; sumHov += sub; }
    }

    int   total  = ss * ss;
    float covSel = (float)nSel / (float)total;
    float covHov = (float)nHov / (float)total;

    // Select takes precedence over hover when both touch this pixel.
    // `ctr` (not `centroid`): `centroid` is a reserved HLSL interpolation modifier.
    float  tag, cov;
    float2 ctr;
    uint   chosenId;
    if (nSel > 0 && covSel >= covHov) { tag =  1.0; cov = covSel; ctr = sumSel / (float)nSel; chosenId = gSelectedId; }
    else if (nHov > 0)                { tag = -1.0; cov = covHov; ctr = sumHov / (float)nHov; chosenId = hoveredId; }
    else return float4(0, 0, 0, 0);   // background: empty seed

    // BOUNDARY-ONLY SEEDING: keep this seed only if the chosen silhouette has an EDGE
    // here -- present in this pixel but NOT filling the dilated (one-subsample-wider)
    // neighborhood. A fully-interior pixel returns empty and is filled by the flood.
    // Out-of-bounds clamps to the edge texel, so a silhouette flush to the viewport
    // crop is treated as interior there (no spurious outline along the crop).
    const int2 idMax = int2(gDim.x * ss - 1, gDim.y * ss - 1);
    bool boundary = false;
    [loop] for (int by = -1; by <= ss; ++by)
    [loop] for (int bx = -1; bx <= ss; ++bx)
    {
        int2 q = clamp(base + int2(bx, by), int2(0, 0), idMax);
        if (gIds.Load(int3(q, 0)) != chosenId) boundary = true;
    }
    if (!boundary) return float4(0, 0, 0, 0);   // silhouette interior: flooded, not seeded

    float2 nrm = (ctr / float2(gDim)) * 2.0 - 1.0;
    return float4(nrm, tag, cov);
}
