#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <execinfo.h>
#include <unistd.h>

#include "otaku/config.hpp"
#include "otaku/frame.hpp"
#include "otaku/hypr.hpp"
#include "otaku/render.hpp"
#include "otaku/wayland.hpp"

using namespace otaku;

namespace {

volatile std::sig_atomic_t g_running = 1;
void on_signal(int) { g_running = 0; }

void on_fatal(int sig) {
    std::fprintf(stderr, "\notakud: fatal signal %d (SIG%s), pid %d\n", sig,
                 (sig == SIGSEGV) ? "SEGV" : (sig == SIGABRT) ? "ABRT" : "?",
                 static_cast<int>(getpid()));
    void* frames[32];
    int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    _exit(128 + sig);
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
    std::signal(SIGSEGV, on_fatal);
    std::signal(SIGABRT, on_fatal);
    std::printf("otakud: starting, pid %d\n", static_cast<int>(getpid()));

    // Build one Frame per configured frame, backed by a layer surface.
    // Step 1 only handles anchored bars (top/bottom/left/right); the
    // fullscreen lockscreen frame arrives in a later step.
    std::vector<std::unique_ptr<Frame>> frames;
    for (const auto& fspec : cfg.frames) {
        // Step 1 only handles anchored bars (top/bottom/left/right); the
        // fullscreen lockscreen frame arrives in a later step.
        if (fspec.anchor == Anchor::None || fspec.anchor == Anchor::Full) continue;

        const auto make_frame = [&](Output* out) {
            auto surf = create_layer_surface(d, out, "otaku-" + fspec.id,
                                             fspec.anchor, cfg.height,
                                             fspec.exclusive);
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

    // Event loop. display_wait sleeps (no busy-loop) until an event arrives
    // or the 100ms tick elapses.
    while (g_running) {
        if (!display_wait(d, 100)) break;
    }

    display_disconnect(d);
    std::printf("otakud: bye\n");
    return 0;
}
