// Pass 2 (run N times, halving gJump from 2^(N-1) down to 1): one jump-flood step.
// For each 1x pixel, among itself + 8 neighbors at +-gJump, keep the seed whose
// stored silhouette position is nearest this pixel. RGBA16_SNORM in/out (ping-pong).

cbuffer JfaCB : register(b0)
{
    int  gJump;   // jump distance (1x px)
    int2 gDim;    // 1x dimensions
    int  _pad;
};

Texture2D<float4> gSeed : register(t0);

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
    int2   p  = int2(i.pos.xy);
    float2 pf = (float2)p + 0.5;

    float4 best  = float4(0, 0, 0, 0);   // .w = coverage; 0 => empty
    float  bestD = 1e20;

    [unroll] for (int oy = -1; oy <= 1; ++oy)
    [unroll] for (int ox = -1; ox <= 1; ++ox)
    {
        int2 q = p + int2(ox, oy) * gJump;
        if (q.x < 0 || q.y < 0 || q.x >= gDim.x || q.y >= gDim.y) continue;
        float4 s = gSeed.Load(int3(q, 0));
        if (s.w <= 0.0) continue;
        float2 sp = (s.xy * 0.5 + 0.5) * float2(gDim);
        float2 dv = sp - pf;
        float  d  = dot(dv, dv);
        if (d < bestD) { bestD = d; best = s; }
    }
    return best;
}
