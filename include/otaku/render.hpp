#pragma once

#include <cstdint>

namespace otaku {

class ISurface;

// Blits a flat rounded rectangle + fills the whole surface in a base color.
// This is the smallest drawing primitive; module rendering is layered on top.
// `radius` rounds the corners. Uses the surface pixel buffer directly.
void render_background(ISurface& s, uint32_t argb, int radius);

// Fills a horizontal strip of `height` px starting at `y` with `argb`.
void render_fill_strip(ISurface& s, int y, int height, uint32_t argb);

}  // namespace otaku
