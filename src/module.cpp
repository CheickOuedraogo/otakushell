#include "otaku/module.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "otaku/render.hpp"
#include "otaku/wayland.hpp"

namespace otaku {

namespace {

// Producers implemented by otakud-mod. Kept in sync with `kProviders` in
// src/otakud-mod.cpp.
const char* const kSupported[] = {"clock",     "sysinfo",  "audio",
                                   "brightness", "workspace", "wifi",
                                   "bluetooth", "app-dock", "systray"};

// A text module: simply shows the producer's latest sample (a "--" placeholder
// while the producer is still starting). Rendering is layout-agnostic, so the
// same code is used by the daemon and the preview.
class TextModule : public IModule {
public:
    TextModule(std::string name, std::shared_ptr<ModuleSource> src)
        : name_(std::move(name)), src_(std::move(src)) {}

    const char* name() const override { return name_.c_str(); }

    // Data is drained by the supervisor's poll_all(); nothing to update here.
    void tick(uint64_t) override {}

    int preferred_width() const override { return 0; }  // layout uses render()

    int measure(const RenderContext& ctx) const override {
        const int w = measure_text(ctx.font, current_text_());
        return w > 0 ? w + kSpacing : 0;
    }

    int render(ISurface& s, const RenderContext& ctx) override {
        const std::string text = current_text_();
        const int w = measure_text(ctx.font, text);
        if (w <= 0) return 0;
        const auto [ascent, descent] = font_metrics(ctx.font);
        const int y = (ctx.height - (ascent + descent)) / 2;
        render_text(s, ctx.x, y, ctx.font, text, ctx.fg);
        return w + kSpacing;
    }

private:
    static constexpr int kSpacing = 20;  // pixels between modules

    std::string current_text_() const {
        if (src_->latest_seq() > 0) return std::string(src_->sample().text);
        return "--";  // producer still starting / not yet published
    }

    std::string name_;
    std::shared_ptr<ModuleSource> src_;
};

}  // namespace

bool module_supported(const std::string& name) {
    for (const char* s : kSupported)
        if (name == s) return true;
    return false;
}

std::shared_ptr<IModule> create_module(const std::string& name,
                                       ModuleSupervisor& sup,
                                       const std::string& opts) {
    if (!module_supported(name)) return nullptr;
    auto src = sup.acquire(name, opts);
    if (!src) return nullptr;
    return std::make_shared<TextModule>(name, std::move(src));
}

}  // namespace otaku