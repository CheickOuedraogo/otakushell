#include "otaku/frame.hpp"

#include <algorithm>

#include "otaku/module.hpp"
#include "otaku/render.hpp"
#include "otaku/wayland.hpp"

namespace otaku {

namespace {

uint32_t color_to_argb(const Color& c) {
    auto byte = [](float v) -> uint32_t {
        const int b = static_cast<int>(v * 255.0f + 0.5f);
        return static_cast<uint32_t>(std::clamp(b, 0, 255));
    };
    return (byte(c.a) << 24) | (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
}

}  // namespace

Frame::Frame(const FrameSpec& spec) : spec_(spec) {
    // Instantiate the modules declared in spec.order.
    for (const auto& mod : spec_.order) {
        if (auto m = create_module(mod, false))
            modules_.push_back(std::move(m));
    }
}

Frame::~Frame() = default;

void Frame::set_theme(const ThemeColors& colors) { theme_ = colors; }

void Frame::set_surface(std::unique_ptr<ISurface> surface) {
    surface_ = std::move(surface);
    if (surface_) surface_->on_resize = [this] { render(); };
}

void Frame::configure(int32_t width, int32_t height) {
    if (!surface_) return;
    surface_->set_size(width, height);
}

void Frame::render() {
    if (!surface_) return;
    if (surface_->visible()) {
        render_background(*surface_, color_to_argb(theme_.background), 10);
    } else {
        // A collapsed auto-hide edge renders fully transparent.
        render_fill_strip(*surface_, 0, surface_->height(), 0u);
    }
    surface_->present();
}

std::vector<std::unique_ptr<Frame>> build_frames(Display& d, const ShellConfig& cfg,
                                                 const std::vector<Output>& outputs) {
    std::vector<std::unique_ptr<Frame>> frames;
    for (const auto& fspec : cfg.frames) {
        // Anchored bars only; the fullscreen lockscreen is a later step.
        if (fspec.anchor == Anchor::None || fspec.anchor == Anchor::Full) continue;

        const auto make_frame = [&](Output* out) {
            auto surf = create_layer_surface(d, out, "otaku-" + fspec.id,
                                             fspec.anchor, cfg.height,
                                             fspec.exclusive, fspec.hidden);
            auto f = std::make_unique<Frame>(fspec);
            f->set_theme(cfg.colors);
            if (fspec.hidden) surf->set_visible(false);  // start collapsed
            f->set_surface(std::move(surf));
            frames.push_back(std::move(f));
        };

        if (cfg.monitor == "auto" && !outputs.empty()) {
            for (const auto& out : outputs) make_frame(const_cast<Output*>(&out));
        } else {
            make_frame(nullptr);  // single surface spanning all monitors
        }
    }
    return frames;
}

}  // namespace otaku