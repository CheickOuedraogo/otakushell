#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <execinfo.h>
#include <sys/types.h>
#include <unistd.h>

#include "otaku/config.hpp"
#include "otaku/frame.hpp"
#include "otaku/hypr.hpp"
#include "otaku/render.hpp"
#include "otaku/wayland.hpp"

using namespace otaku;

namespace {

volatile std::sig_atomic_t g_running = 1;
volatile std::sig_atomic_t g_reload = 0;  // set by SIGUSR1 (hot-reload)

void on_signal(int sig) {
    if (sig == SIGUSR1) {
        g_reload = 1;
        return;
    }
    g_running = 0;
}

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
    std::signal(SIGUSR1, on_signal);
    std::signal(SIGSEGV, on_fatal);
    std::signal(SIGABRT, on_fatal);
    std::printf("otakud: starting, pid %d\n", static_cast<int>(getpid()));

    write_pidfile();  // lets `otakushell reload` find and signal us

    // One Frame per configured frame, backed by a layer surface (step 3:
    // generic frame loading + theme from config; auto-hide edges collapse).
    std::vector<std::unique_ptr<Frame>> frames = build_frames(d, cfg, outputs);
    std::printf("otakud: created %zu frame surface(s)\n", frames.size());
    if (frames.empty())
        std::fprintf(stderr, "otakud: warning: no anchored frames were declared\n");

    // Rebuild a fresh set of frames from the current config + theme.
    const auto apply_frames = [&]() {
        frames.clear();  // destroys old layer surfaces
        frames = build_frames(d, cfg, outputs);
        wl_display_roundtrip(d.display);
        for (auto& f : frames) f->render();
        wl_display_flush(d.display);
    };

    // Wait for layer-shell configure (delivers the real size) and present.
    wl_display_roundtrip(d.display);
    for (auto& f : frames) f->render();
    wl_display_flush(d.display);

    // Event loop. display_wait sleeps (no busy-loop) until an event arrives
    // or the 100ms tick elapses. SIGUSR1 interrupts the poll (EINTR) and sets
    // g_reload, so the next iteration re-reads the config.
    while (g_running) {
        if (g_reload) {
            g_reload = 0;
            ShellConfig next;
            if (load_config(cfg_path, next)) {
                std::printf("otakud: reloading '%s'\n", cfg_path.c_str());
                cfg = std::move(next);
                apply_frames();
                std::printf("otakud: reloaded, %zu frame surface(s)\n",
                            frames.size());
            } else {
                std::fprintf(stderr,
                             "otakud: reload failed (parse error), keeping "
                             "previous config\n");
            }
        }
        if (!display_wait(d, 100)) break;
    }

    remove_pidfile();
    display_disconnect(d);
    std::printf("otakud: bye\n");
    return 0;
}