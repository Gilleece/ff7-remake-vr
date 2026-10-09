// Copies one eye's region of the scene depth into a depth swapchain image: one
// full-screen triangle over the destination, each pixel reading the nearest
// source texel and writing its value as the pixel's depth (depth test always,
// depth writes on, no colour target). The value is passed through unchanged.

cbuffer DepthConstants : register(b0)
{
    float2 srcOrigin;  // top-left texel of the eye's region in the source
    float2 srcScale;   // source texels per destination pixel (region size / destination size)
    float2 srcMax;     // last texel of the region (inclusive), for clamping
    float2 pad;
};

Texture2D<float> depthSrc : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}

float PSMain(float4 pos : SV_Position) : SV_Depth
{
    // pos.xy is the destination pixel centre (x + 0.5); the nearest source texel of
    // the same relative position in the region.
    float2 p = min(floor(srcOrigin + pos.xy * srcScale), srcMax);
    return depthSrc.Load(int3(int2(p), 0));
}
