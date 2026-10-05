// Draws one textured quad given its four corners in clip space (homogeneous,
// so the GPU interpolates the texture coordinates with perspective) and
// clips it against the near plane. Used to composite layers the way a VR
// runtime would: the projection image stretched over the eye's field of view,
// and quad layers placed in space.
//
// Output is LINEAR light; the render target view decides the encoding.

cbuffer QuadConstants : register(b0)
{
    float4 corners[4];  // clip-space positions: top-left, top-right, bottom-left, bottom-right
    float2 uvOffset;    // source UV of the top-left corner
    float2 uvScale;     // source UV extent
    uint   decodeSrgb;  // 1: source values are sRGB-encoded in a UNORM view -> decode
    uint   useAlpha;    // 0: write alpha = 1 (opaque layer)
    uint2  pad;
};

Texture2DArray<float4> src : register(t0);
SamplerState samp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    // Triangle strip: 0 TL, 1 TR, 2 BL, 3 BR.
    float2 t = float2(id & 1, (id >> 1) & 1);
    VSOut o;
    o.pos = corners[id];
    o.uv = uvOffset + t * uvScale;
    return o;
}

float3 SrgbToLinear(float3 c)
{
    c = saturate(c);
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return (c <= 0.04045) ? lo : hi;
}

float4 PSMain(VSOut i) : SV_Target
{
    float4 c = src.Sample(samp, float3(i.uv, 0.0));
    if (decodeSrgb != 0)
        c.rgb = SrgbToLinear(c.rgb);
    if (useAlpha == 0)
        c.a = 1.0;
    return c;
}
