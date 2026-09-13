#include "otaku/render.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include <cairo.h>
#include <pango/pangocairo.h>

#include "otaku/wayland.hpp"

namespace otaku {

void render_fill_strip(ISurface& s, int y, int strip_h, uint32_t argb) {
    const int w = s.width();
    const int h = s.height();
    if (w <= 0 || h <= 0) return;

    uint32_t* px = static_cast<uint32_t*>(s.pixel_data());
    if (!px) return;  // surface not sized/attached yet

    const int y0 = std::max(0, y);
    const int y1 = std::min(h, y + strip_h);
    if (y0 >= y1) return;

    const size_t row_words = static_cast<size_t>(s.stride()) / 4;
    for (int row = y0; row < y1; ++row) {
        uint32_t* p = px + row * row_words;
        for (int x = 0; x < w; ++x) p[x] = argb;
    }
}

// Solid rectangle fill clipped to the surface bounds.
void render_rect(ISurface& s, int x, int y, int w, int h, uint32_t argb) {
    const int sw = s.width();
    const int sh = s.height();
    if (sw <= 0 || sh <= 0) return;
    uint32_t* px = static_cast<uint32_t*>(s.pixel_data());
    if (!px) return;

    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(sw, x + w);
    const int y1 = std::min(sh, y + h);
    if (x0 >= x1 || y0 >= y1) return;

    const size_t row_words = static_cast<size_t>(s.stride()) / 4;
    const int rw = x1 - x0;
    for (int row = y0; row < y1; ++row) {
        uint32_t* p = px + row * row_words + x0;
        for (int i = 0; i < rw; ++i) p[i] = argb;
    }
}

void render_background(ISurface& s, uint32_t argb, int radius) {
    const int w = s.width();
    const int h = s.height();
    if (w <= 0 || h <= 0) return;

    uint32_t* px = static_cast<uint32_t*>(s.pixel_data());
    if (!px) return;  // surface not sized/attached yet
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

// ---------------------------------------------------------------------------
// Text (cairo / pangocairo)
// ---------------------------------------------------------------------------

namespace {

struct PangoGuard {
    cairo_surface_t* cs = nullptr;
    cairo_t* cr = nullptr;
    ~PangoGuard() {
        if (cr) cairo_destroy(cr);
        if (cs) cairo_surface_destroy(cs);
    }
};

PangoLayout* make_layout(cairo_t* cr, const std::string& font,
                         const std::string& text) {
    PangoLayout* lay = pango_cairo_create_layout(cr);
    PangoFontDescription* desc = pango_font_description_from_string(font.c_str());
    pango_layout_set_font_description(lay, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(lay, text.c_str(), -1);
    return lay;
}

// Creates a pango layout + measures ink extents, without touching a surface.
void measure_text_impl(const std::string& font, const std::string& text,
                       PangoRectangle* ink, PangoRectangle* log) {
    PangoGuard g;
    g.cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    g.cr = cairo_create(g.cs);
    PangoLayout* lay = make_layout(g.cr, font, text);
    pango_layout_get_extents(lay, ink, log);
    g_object_unref(lay);
}

}  // namespace

int measure_text(const std::string& font, const std::string& text) {
    if (text.empty()) return 0;
    PangoRectangle ink{}, log{};
    measure_text_impl(font, text, &ink, &log);
    return static_cast<int>((ink.width + PANGO_SCALE / 2) / PANGO_SCALE);
}

std::pair<int, int> font_metrics(const std::string& font) {
    PangoRectangle ink{}, log{};
    measure_text_impl(font, "Ag", &ink, &log);
    const int cell = static_cast<int>(log.height / PANGO_SCALE);
    // Approximate ascent from the ink box: good enough for bar centering.
    const int ascent =
        static_cast<int>((ink.height > 0 ? ink.height + PANGO_SCALE / 2
                                         : log.height + PANGO_SCALE) /
                         PANGO_SCALE);
    return {ascent, cell - ascent};
}

void render_text(ISurface& s, int x, int y, const std::string& font,
                 const std::string& text, uint32_t argb) {
    if (text.empty()) return;
    uint32_t* px = static_cast<uint32_t*>(s.pixel_data());
    if (!px) return;

    PangoGuard g;
    g.cs = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(px), CAIRO_FORMAT_ARGB32, s.width(),
        s.height(), s.stride());
    g.cr = cairo_create(g.cs);

    PangoLayout* lay = make_layout(g.cr, font, text);
    cairo_move_to(g.cr, x, y);
    cairo_set_source_rgba(
        g.cr,
        static_cast<double>((argb >> 16) & 0xff) / 255.0,
        static_cast<double>((argb >> 8) & 0xff) / 255.0,
        static_cast<double>(argb & 0xff) / 255.0,
        static_cast<double>((argb >> 24) & 0xff) / 255.0);
    pango_cairo_show_layout(g.cr, lay);
    g_object_unref(lay);
    cairo_surface_flush(g.cs);
}

}  // namespace otaku