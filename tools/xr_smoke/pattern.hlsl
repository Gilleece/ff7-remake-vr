// Side-by-side stereo test pattern for xr_smoke.
//
// Each eye (left half / right half of the target) gets:
//   * a background in the eye's colour (left: dark red, right: dark blue) with
//     a grey grid every 100 px and a yellow crosshair at the eye centre;
//   * a probe row at the top-left, used by xr_smoke to verify captures:
//       x  40..139  green marker            sRGB (0, 200, 0)   -> orientation
//       x 160..259  eye colour              sRGB (110,40,40) / (40,40,110)
//       x 280..379  mid grey                sRGB (128,128,128) -> gamma
//       x 400..499  1 px black/white lines  (looks like the next patch from afar)
//       x 520..619  sRGB 188 grey           (= 50 % linear light)
//     all at y 40..139;
//   * the eye name ("LEFT" / "RIGHT") and the frame counter in large text;
//   * a 16-step grey ramp along the bottom.
// Colours are authored as sRGB code values. outputLinear = 0 writes them as is
// (like a game back buffer in a UNORM format); outputLinear = 1 writes linear
// light (for _SRGB render target views and float formats).

cbuffer PatternConstants : register(b0)
{
    uint eyeWidth;
    uint eyeHeight;
    uint frame;
    uint outputLinear;
};

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut VSMain(uint id : SV_VertexID)
{
    float2 t = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    return o;
}

// 5x7 font, one uint per row, bit 4 = leftmost pixel.
// 0-9, then L E F T R I G H (10..17), space (18).
static const uint kGlyphs[19 * 7] = {
    0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E,  // 0
    0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E,  // 1
    0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F,  // 2
    0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E,  // 3
    0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02,  // 4
    0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E,  // 5
    0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E,  // 6
    0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08,  // 7
    0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E,  // 8
    0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C,  // 9
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F,  // L
    0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F,  // E
    0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10,  // F
    0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,  // T
    0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11,  // R
    0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E,  // I
    0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F,  // G
    0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11,  // H
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // space
};

bool GlyphPixel(uint glyph, float2 q)  // q in glyph units, origin top-left
{
    if (q.x < 0.0 || q.y < 0.0 || q.x >= 5.0 || q.y >= 7.0)
        return false;
    uint row = (uint)q.y;
    uint col = (uint)q.x;
    return ((kGlyphs[glyph * 7 + row] >> (4 - col)) & 1) != 0;
}

// Text of up to 7 glyphs starting at origin (pixels), glyph pixel size `scale`.
bool TextPixel(float2 p, float2 origin, float scale, uint count, uint g0, uint g1, uint g2, uint g3, uint g4, uint g5, uint g6)
{
    float2 q = (p - origin) / scale;
    if (q.y < 0.0 || q.y >= 7.0 || q.x < 0.0)
        return false;
    uint i = (uint)(q.x / 6.0);
    if (i >= count)
        return false;
    uint glyphs[7] = { g0, g1, g2, g3, g4, g5, g6 };
    return GlyphPixel(glyphs[i], float2(q.x - i * 6.0, q.y));
}

float3 SrgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return (c <= 0.04045) ? lo : hi;
}

float4 PSMain(VSOut i) : SV_Target
{
    uint2 px = (uint2)i.pos.xy;
    uint eye = px.x >= eyeWidth ? 1 : 0;
    float2 p = float2(px.x - eye * eyeWidth, px.y) + 0.5;
    uint2 lp = uint2(px.x - eye * eyeWidth, px.y);
    float W = (float)eyeWidth;
    float H = (float)eyeHeight;

    float3 eyeColour = eye == 0 ? float3(110, 40, 40) : float3(40, 40, 110);
    float3 c = eyeColour;

    // Grid and centre crosshair.
    if ((lp.x % 100) < 2 || (lp.y % 100) < 2)
        c = float3(150, 150, 150);
    if (abs(p.x - W * 0.5) < 3.0 || abs(p.y - H * 0.5) < 3.0)
        c = float3(255, 220, 0);

    // Eye name.
    float labelScale = max(4.0, floor(H / 45.0));
    float labelY = floor(H * 0.25);
    if (eye == 0)
    {
        float w = (4 * 6 - 1) * labelScale;
        if (TextPixel(p, float2(floor((W - w) * 0.5), labelY), labelScale, 4, 10, 11, 12, 13, 18, 18, 18))
            c = float3(255, 255, 255);
    }
    else
    {
        float w = (5 * 6 - 1) * labelScale;
        if (TextPixel(p, float2(floor((W - w) * 0.5), labelY), labelScale, 5, 14, 15, 16, 17, 13, 18, 18))
            c = float3(255, 255, 255);
    }

    // Frame counter: F + 6 digits.
    float numScale = max(3.0, floor(labelScale * 0.5));
    uint f = frame % 1000000;
    uint d0 = (f / 100000) % 10, d1 = (f / 10000) % 10, d2 = (f / 1000) % 10;
    uint d3 = (f / 100) % 10, d4 = (f / 10) % 10, d5 = f % 10;
    float nw = (7 * 6 - 1) * numScale;
    if (TextPixel(p, float2(floor((W - nw) * 0.5), floor(H * 0.62)), numScale, 7, 12, d0, d1, d2, d3, d4, d5))
        c = float3(255, 255, 255);

    // Grey ramp along the bottom: 16 steps of 17.
    float rampTop = H - 120.0, rampBottom = H - 40.0;
    float rampLeft = 160.0, rampRight = W - 160.0;
    if (p.y >= rampTop && p.y < rampBottom && p.x >= rampLeft && p.x < rampRight)
    {
        uint step = (uint)((p.x - rampLeft) / (rampRight - rampLeft) * 16.0);
        c = (float)min(step, 15u) * 17.0;
    }

    // Probe row (drawn last, exact colours).
    if (lp.y >= 40 && lp.y < 140)
    {
        if (lp.x >= 40 && lp.x < 140)
            c = float3(0, 200, 0);
        else if (lp.x >= 160 && lp.x < 260)
            c = eyeColour;
        else if (lp.x >= 280 && lp.x < 380)
            c = float3(128, 128, 128);
        else if (lp.x >= 400 && lp.x < 500)
            c = (lp.y & 1) ? float3(255, 255, 255) : float3(0, 0, 0);
        else if (lp.x >= 520 && lp.x < 620)
            c = float3(188, 188, 188);
    }

    c /= 255.0;
    if (outputLinear != 0)
        c = SrgbToLinear(c);
    return float4(c, 1.0);
}
