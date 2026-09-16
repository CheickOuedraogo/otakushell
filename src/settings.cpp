#include "otaku/settings.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

#include <csignal>

#include "otaku/config.hpp"
#include "otaku/hypr.hpp"
#include "otaku/render.hpp"
#include "otaku/wayland.hpp"

namespace otaku {

// ---------------------------------------------------------------------------
// Graphical settings editor (CLI: `otakushell settings`).
//
// A plain toplevel window drawn entirely with the cairo/shared-memory stack —
// no toolkit. It edits an in-memory ShellConfig, rewrites config.toml on every
// change and nudges the daemon (SIGUSR1) so the bars update live.
//
// Primitive widgets (rows, chips, toggles, sliders, text fields) are drawn
// immediately; click/targets are collected during the render and hit-tested on
// pointer events.
// ---------------------------------------------------------------------------

namespace {

// ---------------------------------------------------------------------------
// UI palette (catppuccin mocha — matches the default theme)
// ---------------------------------------------------------------------------
constexpr uint32_t C_BG = 0xff1e1e2e;
constexpr uint32_t C_PANEL = 0xff181825;
constexpr uint32_t C_ROW = 0xff313244;
constexpr uint32_t C_ROWHI = 0xff45475a;
constexpr uint32_t C_ACC = 0xff89b4fa;
constexpr uint32_t C_TEXT = 0xffcdd6f4;
constexpr uint32_t C_DIM = 0xff6c7086;
constexpr uint32_t C_GOOD = 0xffa6e3a1;
constexpr uint32_t C_BAD = 0xfff38ba8;
constexpr uint32_t C_ONACC = 0xff11111b;

inline uint32_t argb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xff000000u | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

uint32_t argb_from_hex(const char* h) {
    unsigned r = 0, g = 0, b = 0;
    if (std::sscanf(h, "#%2x%2x%2x", &r, &g, &b) == 3)
        return argb_rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                        static_cast<uint8_t>(b));
    return 0xff000000u;
}

uint32_t argb_color(const Color& c) {
    auto byte = [](float v) -> uint8_t {
        return static_cast<uint8_t>(
            std::clamp(static_cast<int>(v * 255.0f + 0.5f), 0, 255));
    };
    return argb_rgb(byte(c.r), byte(c.g), byte(c.b));
}

// Swatches offered by the color pickers.
const char* const kPalette[] = {
    "#1e1e2e", "#181825", "#313244", "#45475a", "#585b70",
    "#cdd6f4", "#a6adc8", "#89b4fa", "#74c7ec", "#a6e3a1",
    "#f9e2af", "#f38ba8", "#eba0ac", "#cba6f7", "#f5c2e7", "#94e2d5",
};

const char* kFontFamilies[] = {"Sans", "Serif", "Monospace"};
const int kFontSizes[] = {10, 11, 12, 14};

const char* kAnchors[] = {"top", "bottom", "left", "right", "full", "none"};

constexpr int NAV_W = 200;
constexpr int CONTENT_X = 212;
constexpr int ROW_H = 28;
constexpr uint32_t BTN_LEFT = 0x110;

const std::string kUIFont = "Sans 11";
const std::string kUIFontH = "Sans 12";

// Known option keys per module kind (shown as labeled text fields).
const std::vector<std::string>& known_options(const std::string& name) {
    static const std::vector<std::string> kClock = {"format", "date_format"};
    static const std::vector<std::string> kSysinfo = {"format"};
    static const std::vector<std::string> kAppDock = {"pinned"};
    static const std::vector<std::string> kNone;
    if (name == "clock") return kClock;
    if (name == "sysinfo") return kSysinfo;
    if (name == "app-dock") return kAppDock;
    return kNone;
}

// Parse an "#rrggbb" swatch into a Color (same grammar as the config loader).
Color color_from_hex(const char* h) {
    const uint32_t v = argb_from_hex(h);
    Color c;
    c.r = static_cast<float>((v >> 16) & 0xff) / 255.0f;
    c.g = static_cast<float>((v >> 8) & 0xff) / 255.0f;
    c.b = static_cast<float>(v & 0xff) / 255.0f;
    return c;
}

// Defined later with the config helpers; declared here because render_module
// uses it to back-fill missing per-module default options.
void seed_module_defaults(ShellConfig& cfg, const std::string& name);

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
enum class View { Theme, Frame, Module };

enum class Drag { None, Height, Interval };

struct SliderGeom {
    int x, y, w;
    Drag kind;
    int mi;  // module index for Interval, -1 for Height
    int min, max;
};

struct Field {
    bool active{false};
    int mi{-1};        // module index (Module view option fields)
    std::string key;   // option key being edited
};

// A clickable rectangle collected during render and hit-tested on pointer up
// (or pressed for sliders). Drawn in the order they are painted; later entries
// win on overlap.
struct HitArea {
    int x, y, w, h;
    std::function<void()> fn;
};

struct App;

struct App {
    Display& d;
    ShellConfig cfg;
    std::string cfg_path;
    ISurface* win{nullptr};

    int W{940}, H{680};
    std::string font_family{"Sans"};
    int font_size{11};

    std::vector<std::string> modules{};  // unique module names in appearance order
    View view{View::Theme};
    int frame_idx{-1};   // selected cfg.frames index
    int module_idx{-1};  // selected index into `modules`

    // UI transient state
    std::vector<HitArea> hits{};
    std::vector<SliderGeom> sliders{};
    Field focus;
    Drag drag{Drag::None};
    int drag_mi{-1};
    std::string status{};
    bool dirty{true};
    bool applying{false};

    // ---- value sources --------------------------------------------------
    int height_val() const { return cfg.height; }
    void set_height(int v) { cfg.height = std::clamp(v, 16, 96); }

    static int default_interval_for(const std::string& name) {
        if (name == "clock") return 1000;
        if (name == "sysinfo") return 2000;
        if (name == "audio") return 500;
        if (name == "brightness") return 1000;
        if (name == "workspace") return 1000;
        if (name == "wifi") return 3000;
        if (name == "bluetooth") return 3000;
        if (name == "app-dock") return 1000;
        if (name == "systray") return 2000;
        return 1000;
    }

    int interval_val(int mi) const {
        const auto& name = modules[static_cast<size_t>(mi)];
        const auto it = cfg.modules.find(name);
        if (it != cfg.modules.end()) {
            const auto o = it->second.find("interval_ms");
            if (o != it->second.end()) return std::atoi(o->second.c_str());
        }
        return default_interval_for(name);
    }
    void set_interval(int mi, int v) {
        cfg.modules[modules[static_cast<size_t>(mi)]]["interval_ms"] =
            std::to_string(v);
    }

    // ---- persistence ----------------------------------------------------
    void apply() {
        if (applying) return;
        applying = true;
        const bool ok = save_config(cfg_path, cfg);
        int pid = 0;
        const bool running = read_pidfile(pid);
        if (running) kill(pid, SIGUSR1);  // hot-reload the daemon's frames
        status = ok ? "config.toml \xE2\x9C\x93"
                    : "config.toml \xE2\x9C\x97 write failed";
        if (running && ok) status += " \xB7 otakud reloaded";
        dirty = true;
        applying = false;
    }

    void commit_text_field() {
        focus.active = false;
        apply();
    }
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
bool hit_contains(const SliderGeom& s, int x, int y) {
    return x >= s.x && x < s.x + s.w && y >= s.y && y < s.y + ROW_H;
}

void draw_row_text(ISurface& s, const std::string& font, int x, int row_y,
                   int row_h, const std::string& text, uint32_t color) {
    const auto [asc, desc] = font_metrics(font);
    const int lh = asc + desc;
    const int ty = row_y + (row_h - lh) / 2 + asc;
    render_text(s, x + 4, ty, font, text, color);
}

void section_label(App& a, int y, const std::string& text) {
    const int sw = measure_text(kUIFont, text);
    render_rect(*a.win, CONTENT_X, y + 3, sw + 8, 18, C_DIM);
    draw_row_text(*a.win, kUIFont, CONTENT_X, y, 24, text, C_BG);
}

void row_label(App& a, int x, int y, const std::string& text, uint32_t color) {
    draw_row_text(*a.win, kUIFont, x, y, ROW_H, text, color);
}

// --- chips (select buttons) ----------------------------------------------
void chip(App& a, int x, int y, const std::string& label, bool selected,
          std::function<void()> fn) {
    const int w = measure_text(kUIFont, label) + 20;
    render_rect(*a.win, x, y, w, 24, selected ? C_ACC : C_ROW);
    draw_row_text(*a.win, kUIFont, x, y, 24, label, selected ? C_ONACC : C_DIM);
    a.hits.push_back({x, y, w, 24, std::move(fn)});
}

// --- toggle switch --------------------------------------------------------
void toggle(App& a, int x, int y, bool on, std::function<void()> fn) {
    const int w = 44, h = 22;
    render_rect(*a.win, x, y, w, h, on ? C_ACC : C_ROWHI);
    const int knob = on ? x + w - 20 : x + 2;
    render_rect(*a.win, knob, y + 2, 18, h - 4, on ? C_ONACC : C_DIM);
    a.hits.push_back({x, y, w, h, std::move(fn)});
}

// --- slider ---------------------------------------------------------------
void slider(App& a, int x, int y, int w, int min, int max, int val, Drag kind,
            int mi) {
    const int ty = y + (ROW_H - 6) / 2;
    render_rect(*a.win, x, ty, w, 6, C_ROWHI);
    const double f =
        max > min ? std::clamp<double>(static_cast<double>(val - min) / (max - min),
                                       0.0, 1.0)
                  : 0.0;
    const int fw = static_cast<int>(f * w);
    render_rect(*a.win, x, ty, fw, 6, C_ACC);
    render_rect(*a.win, x + fw - 3, ty - 4, 6, 14, C_TEXT);
    a.sliders.push_back({x, y, w, kind, mi, min, max});
}

SliderGeom* find_slider(App& a, int x, int y) {
    for (auto it = a.sliders.rbegin(); it != a.sliders.rend(); ++it)
        if (hit_contains(*it, x, y)) return &*it;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Navigation (left sidebar)
// ---------------------------------------------------------------------------
void render_nav(App& a) {
    render_rect(*a.win, 0, 0, NAV_W, a.H, C_PANEL);
    render_rect(*a.win, 0, 0, NAV_W, 40, C_ACC);
    draw_row_text(*a.win, kUIFontH, 6, 0, 40, "otakuShell  settings", C_ONACC);

    const auto nav_item = [&](int row_y, const std::string& text, bool active,
                              std::function<void()> fn) {
        render_rect(*a.win, 8, row_y, NAV_W - 16, ROW_H,
                    active ? C_ROWHI : C_PANEL);
        draw_row_text(*a.win, kUIFont, 14, row_y, ROW_H, text,
                      active ? C_ACC : C_TEXT);
        a.hits.push_back({8, row_y, NAV_W - 16, ROW_H, std::move(fn)});
    };

    int y = 46;
    nav_item(y, "Theme", a.view == View::Theme, [&]() {
        a.view = View::Theme;
        a.frame_idx = a.module_idx = -1;
        a.dirty = true;
    });
    y += ROW_H + 8;

    section_label(a, y, "Frames");
    y += 24;
    for (size_t i = 0; i < a.cfg.frames.size(); ++i) {
        nav_item(y, a.cfg.frames[i].id,
                 a.view == View::Frame && a.frame_idx == static_cast<int>(i),
                 [&, i]() {
                     a.view = View::Frame;
                     a.frame_idx = static_cast<int>(i);
                     a.module_idx = -1;
                     a.dirty = true;
                 });
        y += ROW_H;
    }

    y += 8;
    section_label(a, y, "Modules");
    y += 24;
    for (size_t i = 0; i < a.modules.size(); ++i) {
        const bool en = a.cfg.disabled.count(a.modules[i]) == 0;
        nav_item(y, a.modules[i] + (en ? "" : " \xC2\xB7 off"),
                 a.view == View::Module && a.module_idx == static_cast<int>(i),
                 [&, i]() {
                     a.view = View::Module;
                     a.module_idx = static_cast<int>(i);
                     a.frame_idx = -1;
                     a.dirty = true;
                 });
        y += ROW_H;
    }
}

// ---------------------------------------------------------------------------
// Content header (view title + status)
// ---------------------------------------------------------------------------
void render_header(App& a, const std::string& title) {
    render_rect(*a.win, CONTENT_X, 0, a.W - CONTENT_X, 40, C_BG);
    draw_row_text(*a.win, kUIFontH, CONTENT_X, 4, 28, title, C_TEXT);
    if (!a.status.empty()) {
        const int sw = measure_text(kUIFont, a.status);
        draw_row_text(*a.win, kUIFont, a.W - sw - 12, 4, 28, a.status, C_GOOD);
    }
    render_rect(*a.win, CONTENT_X - 4, 42, a.W - CONTENT_X + 4, 1, C_ROW);
}

// ---------------------------------------------------------------------------
// Theme view
// ---------------------------------------------------------------------------
void render_theme(App& a) {
    render_header(a, "Theme");

    Color* colors[4] = {&a.cfg.colors.background, &a.cfg.colors.foreground,
                        &a.cfg.colors.accent, &a.cfg.colors.muted};
    const char* names[4] = {"Background", "Foreground", "Accent", "Muted"};

    int y = 54;
    section_label(a, y, "Colors");
    y += 24;
    for (int i = 0; i < 4; ++i) {
        row_label(a, CONTENT_X, y, names[i], C_TEXT);
        char hex[16];
        std::snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                      static_cast<int>(colors[i]->r * 255.0f),
                      static_cast<int>(colors[i]->g * 255.0f),
                      static_cast<int>(colors[i]->b * 255.0f));
        row_label(a, CONTENT_X + 120, y, hex, C_DIM);

        int sx = CONTENT_X + 190;
        for (int p = 0; p < 16; ++p, sx += 36) {
            const uint32_t sw = argb_from_hex(kPalette[p]);
            const bool is_cur = argb_color(*colors[i]) == sw;
            if (is_cur) render_rect(*a.win, sx - 1, y + 2, 32, 24, C_TEXT);
            render_rect(*a.win, sx, y + 3, 30, 22, sw);
            a.hits.push_back({sx - 1, y + 2, 32, 24, [&, i, p]() {
                                  const Color c = color_from_hex(kPalette[p]);
                                  *colors[i] = c;
                                  a.apply();
                              }});
        }
        y += ROW_H + 2;
    }

    y += 6;
    section_label(a, y, "Font");
    y += 24;
    row_label(a, CONTENT_X, y, "Family", C_DIM);
    int cx = CONTENT_X + 110;
    for (int f = 0; f < 3; ++f) {
        chip(a, cx, y, kFontFamilies[f], a.font_family == kFontFamilies[f],
             [&, f]() {
                 a.font_family = kFontFamilies[f];
                 a.cfg.font = a.font_family + " " + std::to_string(a.font_size);
                 a.apply();
             });
        cx += measure_text(kUIFont, kFontFamilies[f]) + 32;
    }

    y += ROW_H;
    row_label(a, CONTENT_X, y, "Size", C_DIM);
    cx = CONTENT_X + 110;
    for (int s = 0; s < 4; ++s) {
        char sizelbl[8];
        std::snprintf(sizelbl, sizeof(sizelbl), "%d", kFontSizes[s]);
        chip(a, cx, y, sizelbl, a.font_size == kFontSizes[s], [&, s]() {
            a.font_size = kFontSizes[s];
            a.cfg.font = a.font_family + " " + std::to_string(a.font_size);
            a.apply();
        });
        cx += 26;
    }

    y += ROW_H + 8;
    row_label(a, CONTENT_X, y, "Shell height (px)", C_DIM);
    slider(a, CONTENT_X + 170, y, a.W - CONTENT_X - 300, 20, 60, a.height_val(),
           Drag::Height, -1);
    char hv[16];
    std::snprintf(hv, sizeof(hv), "%d", a.height_val());
    row_label(a, a.W - 70, y, hv, C_ACC);
}

// ---------------------------------------------------------------------------
// Frame view
// ---------------------------------------------------------------------------
void render_frame(App& a) {
    const int fi = a.frame_idx;
    if (fi < 0 || fi >= static_cast<int>(a.cfg.frames.size())) {
        render_header(a, "Frames");
        return;
    }
    FrameSpec& f = a.cfg.frames[static_cast<size_t>(fi)];
    render_header(a, f.id);

    int y = 54;
    row_label(a, CONTENT_X, y, "Anchor", C_DIM);
    int cx = CONTENT_X + 90;
    for (const char* an : kAnchors) {
        const bool sel =
            std::string(an) ==
            (f.anchor == Anchor::Top ? "top"
             : f.anchor == Anchor::Bottom ? "bottom"
             : f.anchor == Anchor::Left  ? "left"
             : f.anchor == Anchor::Right ? "right"
             : f.anchor == Anchor::Full  ? "full"
                                         : "none");
        chip(a, cx, y, an, sel, [&, an]() {
            const auto anchor = parse_anchor(an);
            if (anchor) f.anchor = *anchor;
            a.apply();
        });
        cx += measure_text(kUIFont, an) + 32;
    }

    y += ROW_H + 8;
    row_label(a, CONTENT_X, y, "Exclusive zone", C_TEXT);
    toggle(a, CONTENT_X + 200, y + 3, f.exclusive, [&]() {
        f.exclusive = !f.exclusive;
        a.apply();
    });

    y += ROW_H;
    row_label(a, CONTENT_X, y, "Auto-hide", C_TEXT);
    toggle(a, CONTENT_X + 200, y + 3, f.hidden, [&]() {
        f.hidden = !f.hidden;
        if (f.hidden) f.exclusive = true;
        a.apply();
    });

    y += ROW_H;
    row_label(a, CONTENT_X, y, "Lockscreen frame", C_TEXT);
    toggle(a, CONTENT_X + 200, y + 3, f.lock, [&]() {
        f.lock = !f.lock;
        if (f.lock) f.anchor = Anchor::Full;
        a.apply();
    });

    y += ROW_H + 12;
    section_label(a, y, "Module order");
    y += 24;
    const int bx = CONTENT_X;
    for (size_t i = 0; i < f.order.size(); ++i) {
        render_rect(*a.win, bx, y + 2, 90, 24, C_ROW);
        if (i > 0)  // ▲
            a.hits.push_back({bx, y + 2, 26, 24, [&, i]() {
                                  std::swap(f.order[i], f.order[i - 1]);
                                  a.apply();
                              }});
        if (i + 1 < f.order.size())  // ▼
            a.hits.push_back({bx + 30, y + 2, 26, 24, [&, i]() {
                                  std::swap(f.order[i], f.order[i + 1]);
                                  a.apply();
                              }});
        a.hits.push_back({bx + 60, y + 2, 26, 24, [&, i]() {  // ✕
                              f.order.erase(f.order.begin() +
                                            static_cast<ptrdiff_t>(i));
                              a.apply();
                          }});
        draw_row_text(*a.win, kUIFont, bx + 92, y + 2, 24, f.order[i], C_TEXT);
        y += 30;
    }

    y += 4;
    row_label(a, CONTENT_X, y, "Add:", C_DIM);
    int cx2 = CONTENT_X + 48;
    for (const auto& m : a.modules) {
        if (std::find(f.order.begin(), f.order.end(), m) != f.order.end())
            continue;
        chip(a, cx2, y, m, false, [&, m]() {
            f.order.push_back(m);
            if (a.cfg.modules.count(m) == 0) a.cfg.modules[m] = {};
            a.apply();
        });
        cx2 += measure_text(kUIFont, m) + 32;
        if (cx2 > a.W - 60) break;
    }
}

// ---------------------------------------------------------------------------
// Module view
// ---------------------------------------------------------------------------
void render_module(App& a) {
    const int mi = a.module_idx;
    if (mi < 0 || mi >= static_cast<int>(a.modules.size())) {
        render_header(a, "Modules");
        return;
    }
    const std::string& name = a.modules[static_cast<size_t>(mi)];
    const bool enabled = a.cfg.disabled.count(name) == 0;
    render_header(a, name + (enabled ? "" : " \xC2\xB7 off"));

    int y = 54;

    row_label(a, CONTENT_X, y, "Enabled", C_TEXT);
    toggle(a, CONTENT_X + 200, y + 3, enabled, [&, name]() {
        if (a.cfg.disabled.count(name))
            a.cfg.disabled.erase(name);  // -> enable
        else
            a.cfg.disabled.insert(name);  // -> disable
        set_module_disabled(name, a.cfg.disabled.count(name) > 0);
        a.apply();
    });

    y += ROW_H;
    row_label(a, CONTENT_X, y, "Interval (ms)", C_DIM);
    slider(a, CONTENT_X + 170, y, a.W - CONTENT_X - 300, 250, 10000,
           a.interval_val(mi), Drag::Interval, mi);
    char iv[8];
    std::snprintf(iv, sizeof(iv), "%d", a.interval_val(mi));
    row_label(a, a.W - 70, y, iv, C_ACC);

    // Known, labeled option fields per module kind.
    const std::vector<std::string>& handled = known_options(name);

    y += ROW_H + 10;
    auto& opts = a.cfg.modules[name];
    for (const auto& key : handled) {
        if (opts.count(key) == 0) {
            seed_module_defaults(a.cfg, name);  // fill missing format defaults
            opts = a.cfg.modules[name];
        }
        std::string label = key == "date_format"
                                ? "Date format"
                                : key == "format" ? "Format" : key;
        row_label(a, CONTENT_X, y, label + ":", C_DIM);
        const int fx = CONTENT_X + 170, fw = a.W - fx - 40;
        const bool focused = a.focus.active && a.focus.mi == mi &&
                             a.focus.key == key;
        render_rect(*a.win, fx, y, fw, ROW_H - 4, focused ? C_ROWHI : C_ROW);
        draw_row_text(*a.win, kUIFont, fx, y, ROW_H - 4, opts[key], C_TEXT);
        if (focused) {
            const int tw = opts[key].empty() ? 0
                                             : measure_text(kUIFont, opts[key]);
            render_rect(*a.win, fx + 4 + tw + 2, y + 8, 2, ROW_H - 20, C_TEXT);
        }
        a.hits.push_back({fx, y, fw, ROW_H, [&, key]() {
                              a.focus = {true, mi, key};
                              a.dirty = true;
                          }});
        y += ROW_H + 4;
    }

    // Free-form options (anything else found in config.toml for this module).
    for (auto it = opts.begin(); it != opts.end(); ++it) {
        if (std::find(handled.begin(), handled.end(), it->first) !=
            handled.end())
            continue;
        row_label(a, CONTENT_X, y, it->first + ":", C_DIM);
        const int fx = CONTENT_X + 170, fw = a.W - fx - 70;
        const bool focused = a.focus.active && a.focus.mi == mi &&
                             a.focus.key == it->first;
        render_rect(*a.win, fx, y, fw, ROW_H - 4, focused ? C_ROWHI : C_ROW);
        draw_row_text(*a.win, kUIFont, fx, y, ROW_H - 4, it->second, C_TEXT);
        if (focused) {
            const int tw = it->second.empty() ? 0
                                              : measure_text(kUIFont, it->second);
            render_rect(*a.win, fx + 4 + tw + 2, y + 8, 2, ROW_H - 20, C_TEXT);
        }
        a.hits.push_back({fx, y, fw, ROW_H, [&, key = it->first]() {
                              a.focus = {true, mi, key};
                              a.dirty = true;
                          }});
        a.hits.push_back({fx + fw + 4, y, 26, ROW_H, [&, key = it->first]() {
                              opts.erase(key);
                              if (a.focus.active && a.focus.mi == mi &&
                                  a.focus.key == key)
                                  a.focus.active = false;
                              a.apply();
                          }});
        draw_row_text(*a.win, kUIFont, fx + fw + 4, y, ROW_H, "\xE2\x9C\x95",
                      C_BAD);
        y += ROW_H + 4;
    }
}

// ---------------------------------------------------------------------------
// Keyboard (text fields) — US-layout ASCII from XKB keycodes.
// ---------------------------------------------------------------------------
char ascii_from_key(uint32_t code, bool shift) {
    auto letter = [shift](char lo, char hi) { return shift ? hi : lo; };
    switch (code) {
        case 24: return letter('q', 'Q');
        case 25: return letter('w', 'W');
        case 26: return letter('e', 'E');
        case 27: return letter('r', 'R');
        case 28: return letter('t', 'T');
        case 29: return letter('y', 'Y');
        case 30: return letter('u', 'U');
        case 31: return letter('i', 'I');
        case 32: return letter('o', 'O');
        case 33: return letter('p', 'P');
        case 38: return letter('a', 'A');
        case 39: return letter('s', 'S');
        case 40: return letter('d', 'D');
        case 41: return letter('f', 'F');
        case 42: return letter('g', 'G');
        case 43: return letter('h', 'H');
        case 44: return letter('j', 'J');
        case 45: return letter('k', 'K');
        case 46: return letter('l', 'L');
        case 52: return letter('z', 'Z');
        case 53: return letter('x', 'X');
        case 54: return letter('c', 'C');
        case 55: return letter('v', 'V');
        case 56: return letter('b', 'B');
        case 57: return letter('n', 'N');
        case 58: return letter('m', 'M');
        case 47: return letter(';', ':');
        case 48: return letter('\'', '"');
        case 49: return letter('`', '~');
        case 34: return letter('[', '{');
        case 35: return letter(']', '}');
        case 51: return letter('\\', '|');
        case 10: return shift ? '!' : '1';
        case 11: return shift ? '@' : '2';
        case 12: return shift ? '#' : '3';
        case 13: return shift ? '$' : '4';
        case 14: return shift ? '%' : '5';
        case 15: return shift ? '^' : '6';
        case 16: return shift ? '&' : '7';
        case 17: return shift ? '*' : '8';
        case 18: return shift ? '(' : '9';
        case 19: return shift ? ')' : '0';
        case 20: return shift ? '_' : '-';
        case 21: return shift ? '+' : '=';
        case 59: return letter(',', '<');
        case 60: return letter('.', '>');
        case 61: return letter('/', '?');
        case 65: return ' ';
        default: return 0;
    }
}

void on_key(App& a, uint32_t code, uint32_t state) {
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (!a.focus.active) return;
    if (a.focus.mi < 0 || a.focus.mi >= static_cast<int>(a.modules.size()))
        return;
    auto& opts = a.cfg.modules[a.modules[static_cast<size_t>(a.focus.mi)]];
    auto& value = opts[a.focus.key];

    if (code == 9 || code == 36) {  // Escape / Return → commit
        a.commit_text_field();
        return;
    }
    if (code == 22) {  // Backspace
        if (!value.empty()) value.pop_back();
        a.dirty = true;
        return;
    }
    char c = ascii_from_key(code, (a.d.kbd_mods & 1u) != 0);
    if (c) {
        value.push_back(c);
        a.dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Pointer
// ---------------------------------------------------------------------------
void on_motion(App& a, int x, int y) {
    (void)y;
    if (a.drag == Drag::None) return;
    for (auto& s : a.sliders) {
        if (s.kind != a.drag) continue;
        int v = s.min + (x - s.x) * (s.max - s.min) / s.w;
        v = std::clamp(v, s.min, s.max);
        if (s.kind == Drag::Height) a.set_height(v);
        else a.set_interval(s.mi, v);
        a.dirty = true;
        return;
    }
}

void on_button(App& a, uint32_t button, uint32_t state, int x, int y) {
    if (button != BTN_LEFT) return;
    if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
        const Drag was = a.drag;
        a.drag = Drag::None;
        if (was != Drag::None) a.apply();  // commit the slider at release
        return;
    }
    a.focus.active = false;  // clicking anywhere drops the text focus

    if (SliderGeom* s = find_slider(a, x, y)) {
        a.drag = s->kind;
        a.drag_mi = s->mi;
        int v = s->min + (x - s->x) * (s->max - s->min) / s->w;
        v = std::clamp(v, s->min, s->max);
        if (s->kind == Drag::Height) a.set_height(v);
        else a.set_interval(s->mi, v);
        a.dirty = true;
        return;
    }
    for (size_t i = a.hits.size(); i-- > 0;) {
        const auto& h = a.hits[i];
        if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) {
            h.fn();
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Master render
// ---------------------------------------------------------------------------
void render(App& a) {
    a.hits.clear();
    a.sliders.clear();
    render_background(*a.win, C_BG, 0);
    render_nav(a);
    if (a.view == View::Theme) render_theme(a);
    else if (a.view == View::Frame) render_frame(a);
    else if (a.view == View::Module) render_module(a);
    a.win->present();
}

// ---------------------------------------------------------------------------
// Config helpers
// ---------------------------------------------------------------------------
void font_parse(const std::string& font, std::string& family, int& size) {
    const size_t sp = font.find_last_of(' ');
    if (sp != std::string::npos) {
        const std::string tail = font.substr(sp + 1);
        char* end = nullptr;
        const long n = std::strtol(tail.c_str(), &end, 10);
        if (end && *end == '\0' && end != tail.c_str()) {
            family = font.substr(0, sp);
            size = static_cast<int>(n);
            return;
        }
    }
    family = font;
    size = 11;
}

void seed_module_defaults(ShellConfig& cfg, const std::string& name) {
    auto& opts = cfg.modules[name];
    if (name == "clock") {
        if (opts.count("format") == 0) opts["format"] = "%H:%M";
        if (opts.count("date_format") == 0) opts["date_format"] = "%a %d %b";
    } else if (name == "sysinfo") {
        if (opts.count("format") == 0) opts["format"] = "CPU {cpu}%  MEM {mem}";
    } else if (name == "app-dock") {
        if (opts.count("pinned") == 0) opts["pinned"] = "firefox,kitty,code";
    }
    // Ensure interval is seeded so the slider shows a sane default even before first edit
    if (opts.count("interval_ms") == 0) {
        int d = App::default_interval_for(name);
        // only seed for modules that have a meaningful default (all known)
        opts["interval_ms"] = std::to_string(d);
    }
}

// The module list: every module name referenced by a frame or with options,
// in appearance order, deduplicated.
void collect_modules(App& a) {
    for (const auto& f : a.cfg.frames)
        for (const auto& m : f.order)
            if (std::find(a.modules.begin(), a.modules.end(), m) ==
                a.modules.end())
                a.modules.push_back(m);
    for (const auto& [name, _] : a.cfg.modules)
        if (std::find(a.modules.begin(), a.modules.end(), name) ==
            a.modules.end())
            a.modules.push_back(name);
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int run_settings() {
    std::string cfg_path = getenv("OTAKU_CONFIG") ? getenv("OTAKU_CONFIG")
                                                  : "config.toml";
    ShellConfig cfg;
    if (!load_config(cfg_path, cfg)) {
        std::fprintf(stderr, "otakushell: could not load '%s'\n",
                     cfg_path.c_str());
        return 1;
    }

    Display d;
    std::string err;
    if (!display_connect(d, nullptr, err, /*need_layer_shell=*/false)) {
        std::fprintf(stderr, "otakushell: %s\n", err.c_str());
        std::fprintf(
            stderr,
            "otakushell: settings needs a running Wayland compositor.\n");
        return 1;
    }

    std::atomic<bool> alive{true};
    auto surf = create_toplevel_surface(d, "otakushell settings", 940, 680,
                                        &alive);
    if (!surf) {
        std::fprintf(stderr, "otakushell: failed to create settings window\n");
        display_disconnect(d);
        return 1;
    }

    App a{d, std::move(cfg), cfg_path, surf.get()};
    a.W = surf->width() > 0 ? surf->width() : 940;
    a.H = surf->height() > 0 ? surf->height() : 680;
    font_parse(a.cfg.font, a.font_family, a.font_size);
    collect_modules(a);
    for (const auto& name : a.modules) seed_module_defaults(a.cfg, name);

    surf->on_resize = [&]() {
        a.W = surf->width();
        a.H = surf->height();
        a.dirty = true;
    };

    // Let the compositor deliver the initial configure, then the first frame.
    wl_display_roundtrip(d.display);

    d.on_pointer_motion = [&](wl_surface* s, int x, int y) {
        if (s != surf->native_surface()) return;
        on_motion(a, x, y);
    };
    d.on_pointer_button = [&](wl_surface* s, uint32_t b, uint32_t st, int x,
                              int y) {
        if (s != surf->native_surface()) return;
        on_button(a, b, st, x, y);
    };
    d.on_key = [&](uint32_t k, uint32_t st) { on_key(a, k, st); };

    std::printf("otakushell settings: editing '%s' — close the window to quit.\n",
                cfg_path.c_str());
    std::fflush(stdout);

    render(a);
    wl_display_flush(d.display);

    while (alive.load()) {
        if (a.dirty) {
            render(a);
            wl_display_flush(d.display);
        }
        if (!display_wait(d, 50)) break;
    }

    d.on_pointer_motion = {};
    d.on_pointer_button = {};
    d.on_key = {};
    surf.reset();
    display_disconnect(d);
    std::printf("otakushell settings: bye\n");
    return 0;
}

}  // namespace otaku