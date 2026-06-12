// Output pass: linear HDR canvas -> ACES filmic -> gamma 2.2 encode ->
// display-referred backbuffer. The ACES approximation and 2.2 transfer
// byte-match the LOVE client's post_process.glsl (the porting oracle).
// Fullscreen triangle from SV_VertexID -- no vertex buffer.

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOutput vs_main(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.pos = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}

Texture2D    g_Scene   : register(t0);
SamplerState g_Sampler : register(s0);

// Narkowicz ACES filmic approximation (linear in -> [0,1] out).
float3 ACESFilmic(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 ps_main(VSOutput input) : SV_Target0
{
    float3 linearColor = g_Scene.Sample(g_Sampler, input.uv).rgb;
    float3 display = pow(ACESFilmic(linearColor), 1.0 / 2.2);
    return float4(display, 1.0);
}
