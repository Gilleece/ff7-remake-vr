// A small CPU canvas for the settings panel: rectangles and text in an RGBA8 image with
// premultiplied alpha (what a blended quad layer expects). Text is rasterised with
// stb_truetype from a font installed with Windows (Segoe UI, or Arial), found at run time
// in the Windows fonts folder; nothing is shipped.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ff7vr::render::menu {

struct Rgba {
    uint8_t r = 0, g = 0, b = 0;
    float a = 1.0f;  // 0..1
};

class Canvas {
public:
    Canvas();
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    // Loads the font once; false when no font was found (text is then not drawn).
    bool LoadFont(std::string* which);
    bool HasFont() const { return font_ != nullptr; }

    void Resize(uint32_t w, uint32_t h);
    uint32_t Width() const { return w_; }
    uint32_t Height() const { return h_; }
    const std::vector<uint8_t>& Pixels() const { return px_; }

    void Clear();  // fully transparent
    void FillRect(int x, int y, int w, int h, Rgba c);
    // Draws UTF-8 text with its baseline at y; returns the advance width in pixels.
    int Text(int x, int baseline, std::string_view utf8, int sizePx, Rgba c);
    int TextWidth(std::string_view utf8, int sizePx);
    // Ascent of the font at this size (pixels above the baseline).
    int Ascent(int sizePx);

private:
    struct Glyph {
        int w = 0, h = 0, xoff = 0, yoff = 0;
        float advance = 0;
        std::vector<uint8_t> bitmap;
    };
    const Glyph& GetGlyph(uint32_t codepoint, int sizePx);
    int Draw(int x, int baseline, std::string_view utf8, int sizePx, const Rgba* c);

    uint32_t w_ = 0, h_ = 0;
    std::vector<uint8_t> px_;
    std::vector<uint8_t> fontData_;
    void* font_ = nullptr;  // stbtt_fontinfo
    std::map<uint64_t, Glyph> glyphs_;
};

}  // namespace ff7vr::render::menu
