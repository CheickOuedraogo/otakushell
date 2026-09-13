#include <algorithm>
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
#include "otaku/shm.hpp"
#include "otaku/supervisor.hpp"
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

uint64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Status region: written by the daemon, read by `otakushell status`.
StatusRegion* g_status = nullptr;
void* g_status_region = nullptr;

void status_reset(const char* version, uint64_t start_ns) {
    g_status_region = shm_open_region("status", kStatusRegionSize, /*create=*/true);
    if (!g_status_region) return;
    std::memset(g_status_region, 0, kStatusRegionSize);
    g_status = ::new (g_status_region) StatusRegion{};
    g_status->magic.store(kStatusMagic, std::memory_order_relaxed);
    std::snprintf(g_status->version, sizeof(g_status->version), "%s", version);
    g_status->start_ns = start_ns;
}

void status_update(const std::vector<std::unique_ptr<Frame>>& frames,
                   const ModuleSupervisor& sup) {
    if (!g_status) return;
    g_status->frame_count = static_cast<uint32_t>(frames.size());

    const auto entries = sup.entries();
    const uint32_t n = static_cast<uint32_t>(
        std::min<size_t>(entries.size(), kStatusMaxModules));
    for (uint32_t i = 0; i < n; ++i) {
        std::snprintf(g_status->modules[i].name,
                      sizeof(g_status->modules[i].name), "%s",
                      entries[i].first.c_str());
        g_status->modules[i].pid = entries[i].second;
    }
    g_status->module_count.store(n, std::memory_order_relaxed);
}

void status_clear() {
    if (g_status_region) {
        shm_close_region(g_status_region, kStatusRegionSize, "status",
                         /*unlink=*/true);
        g_status_region = nullptr;
        g_status = nullptr;
    }
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

    ModuleSupervisor supervisor(/*simulate=*/false);
    status_reset(OTAKU_VERSION, mono_ns());

    // One Frame per configured frame, backed by a layer surface (step 3:
    // generic frame loading + theme from config; auto-hide edges collapse).
    std::vector<std::unique_ptr<Frame>> frames =
        build_frames(d, supervisor, cfg, outputs);
    std::printf("otakud: created %zu frame surface(s)\n", frames.size());
    if (frames.empty())
        std::fprintf(stderr, "otakud: warning: no anchored frames were declared\n");

    // Rebuild a fresh set of frames from the current config + theme.
    const auto apply_frames = [&]() {
        frames.clear();  // destroys old layer surfaces
        frames = build_frames(d, supervisor, cfg, outputs);
        wl_display_roundtrip(d.display);
        for (auto& f : frames) f->render();
        wl_display_flush(d.display);
    };

    // Wait for layer-shell configure (delivers the real size) and present.
    wl_display_roundtrip(d.display);
    for (auto& f : frames) f->render();
    wl_display_flush(d.display);

    // Event loop. display_wait sleeps (no busy-loop) until an event arrives
    // or the interval elapses. SIGUSR1 interrupts the poll (EINTR) and sets
    // g_reload, so the next iteration re-reads the config.
    uint64_t last_reap = mono_ns();
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

        // Modules: drain producer rings and repaint once a second at most.
        const uint64_t now = mono_ns();
        supervisor.poll_all(now);
        if (now - last_reap > 5'000'000'000ull) {
            supervisor.reap();
            last_reap = now;
        }
        status_update(frames, supervisor);

        // Repaint every ~500 ms so clock/sysinfo values stay fresh.
        const int wait_ms = 100;
        if (g_reload == 0) {
            static uint64_t last_paint = 0;
            if (now - last_paint >= 500'000'000ull) {
                last_paint = now;
                for (auto& f : frames) f->render();
                wl_display_flush(d.display);
            }
        }

        if (!display_wait(d, wait_ms)) break;
    }

    supervisor.shutdown();
    status_clear();
    remove_pidfile();
    display_disconnect(d);
    std::printf("otakud: bye\n");
    return 0;
}