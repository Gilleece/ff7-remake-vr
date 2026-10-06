// Full-screen-triangle blit with source sub-rectangle and colour decoding.
// Output is always LINEAR light; the render target view decides the encoding
// (an _SRGB RTV encodes in hardware; a UNORM/float RTV stores linear values,
// which is how OpenXR runtimes interpret non-sRGB swapchain formats).

cbuffer BlitConstants : register(b0)
{
    float2 uvOffset;   // source UV of the destination's top-left corner
    float2 uvScale;    // source UV extent of the destination
    uint   decodeSrgb; // 1: source values are sRGB-encoded in a UNORM view -> decode
    uint   alphaMode;  // 0: write alpha = 1; 1: keep (premultiplied); 2: straight -> premultiplied;
                       // 3: inverted premultiplied (alpha = 1 - coverage) -> premultiplied
    uint   encodeSrgb; // 1: the target stores gamma-encoded values in a non-sRGB view -> encode
                       //    (premultiplied output: rgb = encode(rgb / a) * a, blended in gamma space)
    uint   pad;
    // Picture adjustment (PictureAdjust in xr.h), used when picture != 0 and the
    // output is opaque (alphaMode 0). pic0: saturation, then the curve texture's
    // coordinate scale and offset (texel centres).
    float4 pic0;
    float4 pic1;
    uint   picture;
    uint3  pad2;
};

Texture2DArray<float4> src : register(t0);
SamplerState samp : register(s0);
// The per-channel part of the picture adjustment (brightness, contrast, gamma, black
// level; d3d11_blitter.cpp, PictureCurve) as a curve over t = sqrt(linear / 2), read
// with linear filtering. One texture read per channel instead of four powers: the
// adjustment's cost stays below what the blit's timing can resolve.
Texture1D<float> curve : register(t1);
SamplerState curveSamp : register(s1);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    // 0:(0,0) 1:(2,0) 2:(0,2) in UV; covers the viewport with one triangle.
    float2 t = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    o.uv = t;
    return o;
}

float3 SrgbToLinear(float3 c)
{
    c = saturate(c);
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return (c <= 0.04045) ? lo : hi;  // component-wise in SM5 / fxc
}

float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return (c <= 0.0031308) ? lo : hi;
}

// Linear in, linear out. Saturation mixes towards the pixel's luminance (Rec. 709),
// which it keeps; the rest is the per-channel curve. Saturation is applied before the
// curve's brightness gain: both are linear in light, so the order does not matter.
float3 AdjustPicture(float3 lin)
{
    float y = dot(lin, float3(0.2126, 0.7152, 0.0722));
    lin = max(y + (lin - y) * pic0.x, 0.0);
    float3 u = sqrt(lin * 0.5) * pic0.y + pic0.z;
    return float3(curve.SampleLevel(curveSamp, u.r, 0.0), curve.SampleLevel(curveSamp, u.g, 0.0),
                  curve.SampleLevel(curveSamp, u.b, 0.0));
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = uvOffset + i.uv * uvScale;
    float4 c = src.SampleLevel(samp, float3(uv, 0.0), 0.0);
    if (decodeSrgb != 0)
        c.rgb = SrgbToLinear(c.rgb);
    if (picture != 0 && alphaMode == 0)
        c.rgb = AdjustPicture(c.rgb);
    if (alphaMode == 0)
        c.a = 1.0;
    else if (alphaMode == 2)
        c.rgb *= c.a;
    else if (alphaMode == 3)
        c.a = 1.0 - c.a;
    if (encodeSrgb != 0)
        c.rgb = (alphaMode == 0) ? LinearToSrgb(c.rgb) : (c.a > 1e-5 ? LinearToSrgb(c.rgb / c.a) * c.a : 0.0);
    return c;
}
