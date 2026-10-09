#include "menu_canvas.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#pragma warning(push, 0)
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#pragma warning(pop)

namespace ff7vr::render::menu {
namespace {

// Next code point of a UTF-8 string (invalid bytes come out as '?').
uint32_t NextCodepoint(std::string_view s, size_t* i) {
    const auto c = static_cast<unsigned char>(s[(*i)++]);
    if (c < 0x80) return c;
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0) return '?';
    uint32_t cp = c & (0x3F >> extra);
    while (extra-- > 0) {
        if (*i >= s.size() || (static_cast<unsigned char>(s[*i]) & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (static_cast<unsigned char>(s[(*i)++]) & 0x3F);
    }
    return cp;
}

stbtt_fontinfo* Info(void* p) { return static_cast<stbtt_fontinfo*>(p); }

}  // namespace

Canvas::Canvas() = default;

Canvas::~Canvas() { delete Info(font_); }

bool Canvas::LoadFont(std::string* which) {
    if (font_) return true;
    wchar_t windir[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryW(windir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    for (const wchar_t* name : {L"\\Fonts\\segoeui.ttf", L"\\Fonts\\arial.ttf"}) {
        const std::wstring path = std::wstring(windir) + name;
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string data = ss.str();
        if (data.size() < 1024) continue;
        fontData_.assign(data.begin(), data.end());
        auto* info = new stbtt_fontinfo{};
        if (!stbtt_InitFont(info, fontData_.data(), stbtt_GetFontOffsetForIndex(fontData_.data(), 0))) {
            delete info;
            fontData_.clear();
            continue;
        }
        font_ = info;
        if (which) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<size_t>(std::max(0, len - 1)), '\0');
            WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, out.data(), len, nullptr, nullptr);
            *which = out;
        }
        return true;
    }
    return false;
}

void Canvas::Resize(uint32_t w, uint32_t h) {
    w_ = w;
    h_ = h;
    px_.assign(size_t(w) * h * 4, 0);
}

void Canvas::Clear() { std::fill(px_.begin(), px_.end(), uint8_t(0)); }

void Canvas::FillRect(int x, int y, int w, int h, Rgba c) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(int(w_), x + w), y1 = std::min(int(h_), y + h);
    const float a = std::clamp(c.a, 0.0f, 1.0f);
    const float sr = c.r * a, sg = c.g * a, sb = c.b * a, sa = 255.0f * a;
    for (int yy = y0; yy < y1; ++yy) {
        uint8_t* p = px_.data() + (size_t(yy) * w_ + size_t(x0)) * 4;
        for (int xx = x0; xx < x1; ++xx, p += 4) {
            p[0] = static_cast<uint8_t>(std::lround(sr + p[0] * (1.0f - a)));
            p[1] = static_cast<uint8_t>(std::lround(sg + p[1] * (1.0f - a)));
            p[2] = static_cast<uint8_t>(std::lround(sb + p[2] * (1.0f - a)));
            p[3] = static_cast<uint8_t>(std::lround(sa + p[3] * (1.0f - a)));
        }
    }
}

const Canvas::Glyph& Canvas::GetGlyph(uint32_t cp, int sizePx) {
    const uint64_t key = (uint64_t(uint32_t(sizePx)) << 32) | cp;
    auto it = glyphs_.find(key);
    if (it != glyphs_.end()) return it->second;
    Glyph g;
    stbtt_fontinfo* f = Info(font_);
    const float scale = stbtt_ScaleForPixelHeight(f, float(sizePx));
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(f, int(cp), &adv, &lsb);
    g.advance = adv * scale;
    unsigned char* bmp = stbtt_GetCodepointBitmap(f, scale, scale, int(cp), &g.w, &g.h, &g.xoff, &g.yoff);
    if (bmp) {
        g.bitmap.assign(bmp, bmp + size_t(g.w) * size_t(g.h));
        stbtt_FreeBitmap(bmp, nullptr);
    }
    return glyphs_.emplace(key, std::move(g)).first->second;
}

int Canvas::Ascent(int sizePx) {
    if (!font_) return sizePx * 3 / 4;
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(Info(font_), &ascent, &descent, &gap);
    return int(std::lround(ascent * stbtt_ScaleForPixelHeight(Info(font_), float(sizePx))));
}

int Canvas::Draw(int x, int baseline, std::string_view s, int sizePx, const Rgba* c) {
    if (!font_) return 0;
    stbtt_fontinfo* f = Info(font_);
    const float scale = stbtt_ScaleForPixelHeight(f, float(sizePx));
    float pen = float(x);
    uint32_t prev = 0;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = NextCodepoint(s, &i);
        if (prev) pen += stbtt_GetCodepointKernAdvance(f, int(prev), int(cp)) * scale;
        const Glyph& g = GetGlyph(cp, sizePx);
        if (c && !g.bitmap.empty()) {
            const int gx = int(std::lround(pen)) + g.xoff, gy = baseline + g.yoff;
            const float a = std::clamp(c->a, 0.0f, 1.0f);
            for (int yy = 0; yy < g.h; ++yy) {
                const int py = gy + yy;
                if (py < 0 || py >= int(h_)) continue;
                for (int xx = 0; xx < g.w; ++xx) {
                    const int px = gx + xx;
                    if (px < 0 || px >= int(w_)) continue;
                    const float cov = g.bitmap[size_t(yy) * size_t(g.w) + size_t(xx)] / 255.0f * a;
                    if (cov <= 0.0f) continue;
                    uint8_t* p = px_.data() + (size_t(py) * w_ + size_t(px)) * 4;
                    p[0] = static_cast<uint8_t>(std::lround(c->r * cov + p[0] * (1.0f - cov)));
                    p[1] = static_cast<uint8_t>(std::lround(c->g * cov + p[1] * (1.0f - cov)));
                    p[2] = static_cast<uint8_t>(std::lround(c->b * cov + p[2] * (1.0f - cov)));
                    p[3] = static_cast<uint8_t>(std::lround(255.0f * cov + p[3] * (1.0f - cov)));
                }
            }
        }
        pen += g.advance;
        prev = cp;
    }
    return int(std::lround(pen)) - x;
}

int Canvas::Text(int x, int baseline, std::string_view utf8, int sizePx, Rgba c) { return Draw(x, baseline, utf8, sizePx, &c); }

int Canvas::TextWidth(std::string_view utf8, int sizePx) { return Draw(0, 0, utf8, sizePx, nullptr); }

}  // namespace ff7vr::render::menu
