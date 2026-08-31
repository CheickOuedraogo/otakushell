#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "otaku/config.hpp"
#include "otaku/frame.hpp"
#include "otaku/hypr.hpp"
#include "otaku/render.hpp"
#include "otaku/wayland.hpp"

using namespace otaku;

namespace {

volatile std::sig_atomic_t g_running = 1;
void on_signal(int) { g_running = 0; }

// Map our Anchor to wlr-layer-shell anchor flags.
uint32_t anchor_flags(Anchor a) {
    switch (a) {
        case Anchor::Top: return 1 /*TOP*/;
        case Anchor::Bottom: return 2 /*BOTTOM*/;
        case Anchor::Left: return 4 /*LEFT*/;
        case Anchor::Right: return 8 /*RIGHT*/;
        case Anchor::Full: return 0;
        case Anchor::None: return 0;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string cfg_path = getenv("OTAKU_CONFIG") ? getenv("OTAKU_CONFIG")
                                                  : "config.toml";
    std::string err;

    ShellConfig cfg;
    if (!load_config(cfg_path, cfg)) {
        std::fprintf(stderr, "otakud: could not load config from '%s'\n",
                     cfg_path.c_str());
        return 1;
    }

    // Take over an existing shell running on the desktop (only when a
    // Hyprland session is present, so we never kill the user's shell when
    // otakuShell can't actually start). The report is printed inside
    // maim_existing_shell.
    if (cfg.take_over && hyprland_available()) {
        maim_existing_shell(cfg.take_over_kill, cfg.take_over_dry_run);
    }

    Display d;
    std::vector<Output> outputs;
    if (!display_connect(d, &outputs, err)) {
        std::fprintf(stderr, "otakud: %s\n", err.c_str());
        std::fprintf(stderr,
                     "otakud: otakuShell requires a running Wayland compositor "
                     "(Hyprland).\n");
        return 1;
    }
    std::printf("otakud (%s): connected, %zu output(s)\n", OTAKU_VERSION,
                outputs.size());
    for (const auto& o : outputs)
        std::printf("  - output %u\n", o.name);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Build one Frame per configured frame, backed by a layer surface.
    // Step 1 only handles anchored bars (top/bottom/left/right); the
    // fullscreen lockscreen frame arrives in a later step.
    std::vector<std::unique_ptr<Frame>> frames;
    for (const auto& fspec : cfg.frames) {
        uint32_t flags = anchor_flags(fspec.anchor);
        if (flags == 0) continue;  // unanchored/full frames deferred

        const auto make_frame = [&](Output* out) {
            auto surf = create_layer_surface(d, out, "otaku-" + fspec.id, flags,
                                             cfg.height, fspec.exclusive);
            auto f = std::make_unique<Frame>(fspec);
            f->set_surface(std::move(surf));
            frames.push_back(std::move(f));
        };

        if (cfg.monitor == "auto" && !outputs.empty()) {
            for (auto& out : outputs) make_frame(&out);
        } else {
            make_frame(nullptr);  // single surface for now (all monitors)
        }
    }

    std::printf("otakud: created %zu frame surface(s)\n", frames.size());
    if (frames.empty())
        std::fprintf(stderr, "otakud: warning: no anchored frames were declared\n");

    // Wait for layer-shell configure (delivers the real size) and present.
    wl_display_roundtrip(d.display);
    for (auto& f : frames) f->render();
    wl_display_flush(d.display);

    while (g_running && display_poll(d)) {
        // Event loop: keeps surfaces alive and processes input.
    }

    display_disconnect(d);
    std::printf("otakud: bye\n");
    return 0;
}
