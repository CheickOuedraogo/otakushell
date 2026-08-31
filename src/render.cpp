#include "otaku/render.hpp"

#include <algorithm>
#include <cstring>

#include "otaku/wayland.hpp"

namespace otaku {

void render_fill_strip(ISurface& s, int y, int strip_h, uint32_t argb) {
    const int w = s.width();
    const int h = s.height();
    if (w <= 0 || h <= 0) return;

    const int y0 = std::max(0, y);
    const int y1 = std::min(h, y + strip_h);
    if (y0 >= y1) return;

    uint32_t* px = static_cast<uint32_t*>(s.pixel_data());
    const size_t row_words = static_cast<size_t>(s.stride()) / 4;
    for (int row = y0; row < y1; ++row) {
        uint32_t* p = px + row * row_words;
        for (int x = 0; x < w; ++x) p[x] = argb;
    }
}

void render_background(ISurface& s, uint32_t argb, int radius) {
    const int w = s.width();
    const int h = s.height();
    if (w <= 0 || h <= 0) return;

    uint32_t* px = static_cast<uint32_t*>(s.pixel_data());
    const size_t row_words = static_cast<size_t>(s.stride()) / 4;
    const int r = std::max(0, std::min(radius, std::min(w, h) / 2));

    const uint8_t a = (argb >> 24) & 0xff;
    const uint8_t rr = (argb >> 16) & 0xff;
    const uint8_t gg = (argb >> 8) & 0xff;
    const uint8_t bb = argb & 0xff;

    for (int y = 0; y < h; ++y) {
        uint32_t* p = px + y * row_words;
        for (int x = 0; x < w; ++x) {
            // Simple rounded-corner coverage: skip pixels outside the corner arcs.
            bool corner = false;
            uint32_t val;
            if (r > 0) {
                int cx = x, cy = y;
                if (x < r && y < r) { cx = r - x; cy = r - y; corner = true; }
                else if (x >= w - r && y < r) { cx = x - (w - r - 1); cy = r - y; corner = true; }
                else if (x < r && y >= h - r) { cx = r - x; cy = y - (h - r - 1); corner = true; }
                else if (x >= w - r && y >= h - r) { cx = x - (w - r - 1); cy = y - (h - r - 1); corner = true; }

                if (corner) {
                    const int dist2 = cx * cx + cy * cy;
                    const int r2 = r * r;
                    if (dist2 > r2) { p[x] = 0; continue; }
                    val = (a << 24) | (rr << 16) | (gg << 8) | bb;
                    p[x] = val;
                    continue;
                }
            }
            val = (a << 24) | (rr << 16) | (gg << 8) | bb;
            p[x] = val;
        }
    }
}

}  // namespace otaku
