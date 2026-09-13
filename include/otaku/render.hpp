#pragma once

#include <cstdint>
#include <string>

namespace otaku {

class ISurface;

// Blits a flat rounded rectangle + fills the whole surface in a base color.
// This is the smallest drawing primitive; module rendering is layered on top.
// `radius` rounds the corners. Uses the surface pixel buffer directly.
void render_background(ISurface& s, uint32_t argb, int radius);

// Fills a horizontal strip of `height` px starting at `y` with `argb`.
void render_fill_strip(ISurface& s, int y, int height, uint32_t argb);

// --- Text (cairo / pangocairo) ----------------------------------------------

// Measures the pixel width of `text` laid out with the pango font description
// `font` (e.g. "Sans 11"). Used for horizontal module layout.
int measure_text(const std::string& font, const std::string& text);

// Vertical metrics of `font` (ascent, descent) for centering in a bar.
std::pair<int, int> font_metrics(const std::string& font);

// Draws `text` in color `argb` with its glyph box top-left at pixel (x, y).
void render_text(ISurface& s, int x, int y, const std::string& font,
                 const std::string& text, uint32_t argb);

}  // namespace otaku