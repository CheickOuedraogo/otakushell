#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <csignal>
#include <unistd.h>

#include "otaku/config.hpp"
#include "otaku/frame.hpp"
#include "otaku/hypr.hpp"
#include "otaku/shm.hpp"
#include "otaku/supervisor.hpp"
#include "otaku/wayland.hpp"
#include "otaku/settings.hpp"

using namespace otaku;

namespace {
void usage() {
    std::printf(
        "otakushell - otakuShell control CLI\n"
        "\n"
        "usage:\n"
        "  otakushell preview [--list|--all] [frame-id]  open a window previewing a frame\n"
        "  otakushell settings              open the graphical settings editor\n"
        "  otakushell reload               hot-reload config\n"
        "  otakushell status               show active frames/modules\n"
        "  otakushell module enable|disable <name>\n"
        "  otakushell lock | unlock         toggle the session lockscreen\n"
        "  otakushell exec <cmd>\n");
}

uint64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

int do_reload() {
    int pid = 0;
    if (!read_pidfile(pid)) {
        std::fprintf(stderr,
                     "otakushell: could not find otakud PID — is it running?\n");
        return 1;
    }
    if (kill(pid, SIGUSR1) != 0) {
        std::fprintf(stderr, "otakushell: failed to signal otakud (pid %d)\n",
                     pid);
        return 1;
    }
    std::printf("otakushell: signaled otakud (pid %d) to reload config\n", pid);
    return 0;
}

int do_lock() {
    int pid = 0;
    if (!read_pidfile(pid)) {
        std::fprintf(stderr,
                     "otakushell: could not find otakud PID — is it running?\n");
        return 1;
    }
    if (kill(pid, SIGUSR2) != 0) {
        std::fprintf(stderr, "otakushell: failed to signal otakud (pid %d)\n",
                     pid);
        return 1;
    }
    std::printf("otakushell: signaled otakud (pid %d) to toggle the session lock\n",
                pid);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    const std::string cmd = argv[1];

    if (cmd == "preview") {
        std::string cfg_path = getenv("OTAKU_CONFIG") ? getenv("OTAKU_CONFIG")
                                                       : "config.toml";
        ShellConfig cfg;
        if (!load_config(cfg_path, cfg)) {
            std::fprintf(stderr, "otakushell: could not load '%s'\n", cfg_path.c_str());
            return 1;
        }

        // --list: show previewable frames
        if (argc >= 3 && (std::string(argv[2]) == "--list" || std::string(argv[2]) == "-l")) {
            std::printf("previewable frames (anchor != full/none):\n");
            for (const auto& f : cfg.frames) {
                if (f.anchor == Anchor::Full || f.anchor == Anchor::None) continue;
                const char* orient = (f.anchor == Anchor::Top || f.anchor == Anchor::Bottom)
                                     ? "horizontal" : "vertical";
                std::printf("  %-18s %s  order:", f.id.c_str(), orient);
                for (const auto& m : f.order) std::printf(" %s", m.c_str());
                std::printf("\n");
            }
            return 0;
        }

        // Collect target frame specs (single vs --all)
        std::vector<const FrameSpec*> targets;
        const bool all = (argc >= 3 && (std::string(argv[2]) == "--all" || std::string(argv[2]) == "-a"));
        if (all) {
            for (const auto& f : cfg.frames)
                if (f.anchor != Anchor::Full && f.anchor != Anchor::None)
                    targets.push_back(&f);
            if (targets.empty()) {
                std::fprintf(stderr, "otakushell: no previewable frames (all are full/none)\n");
                return 1;
            }
        } else {
            const std::string wanted = (argc >= 3) ? argv[2] : "navbar-top";
            // allow `preview --all <ignored>` with extra arg?
            const FrameSpec* fspec = nullptr;
            for (const auto& f : cfg.frames) if (f.id == wanted) { fspec = &f; break; }
            if (!fspec) {
                std::fprintf(stderr, "otakushell: unknown frame '%s' (see config.toml or --list)\n",
                             wanted.c_str());
                return 1;
            }
            if (fspec->anchor == Anchor::Full || fspec->anchor == Anchor::None) {
                std::fprintf(stderr,
                             "otakushell: frame '%s' cannot be previewed in a window "
                             "(anchors full/none are for the desktop; --list to see previewable)\n",
                             fspec->id.c_str());
                return 1;
            }
            targets.push_back(fspec);
        }

        // The preview needs only xdg-shell, not layer-shell.
        Display d;
        std::atomic<bool> alive{true};
        std::string err;
        if (!display_connect(d, nullptr, err, /*need_layer_shell=*/false)) {
            std::fprintf(stderr, "otakushell: %s\n", err.c_str());
            std::fprintf(stderr,
                         "otakushell: preview needs a running Wayland compositor.\n");
            return 1;
        }

        constexpr int32_t kMockScreen = 1280;
        ModuleSupervisor supervisor(/*simulate=*/true);  // mocked data
        std::vector<std::unique_ptr<Frame>> frames;
        frames.reserve(targets.size());

        for (const FrameSpec* fspec : targets) {
            const bool horizontal = (fspec->anchor == Anchor::Top ||
                                      fspec->anchor == Anchor::Bottom);
            const int32_t win_w = horizontal ? kMockScreen : cfg.height;
            const int32_t win_h = horizontal ? cfg.height : kMockScreen;
            auto surf = create_toplevel_surface(
                d, "otakushell preview \xe2\x80\x94 " + fspec->id, win_w, win_h, &alive);
            if (!surf) {
                std::fprintf(stderr, "otakushell: failed to create preview window for '%s'\n",
                             fspec->id.c_str());
                continue;
            }
            auto fr = std::make_unique<Frame>(*fspec);
            fr->set_theme(cfg.colors);
            fr->attach_modules(supervisor, cfg);
            fr->set_surface(std::move(surf));
            frames.push_back(std::move(fr));
        }

        if (frames.empty()) {
            std::fprintf(stderr, "otakushell: failed to create preview window(s)\n");
            display_disconnect(d);
            supervisor.shutdown();
            return 1;
        }

        // Let the compositor deliver the initial configure, then first frame.
        wl_display_roundtrip(d.display);
        for (auto& f : frames) f->render();
        wl_display_flush(d.display);

        if (targets.size() == 1) {
            const bool h = (targets[0]->anchor == Anchor::Top || targets[0]->anchor == Anchor::Bottom);
            std::printf("otakushell preview (%s): frame '%s' (%s)\n", OTAKU_VERSION,
                        targets[0]->id.c_str(), h ? "top/bottom bar" : "side edge");
        } else {
            std::printf("otakushell preview (%s): %zu frames", OTAKU_VERSION, frames.size());
            for (auto* t : targets) std::printf(" %s", t->id.c_str());
            std::printf("\n");
        }
        std::printf("otakushell: close any window to quit.\n");
        std::fflush(stdout);

        uint64_t last_paint = mono_ns();
        while (alive.load()) {
            const uint64_t now = mono_ns();
            supervisor.poll_all(now);
            // Repaint ~500ms like the daemon so clock/sysinfo/wifi stay fresh
            if (now - last_paint >= 500'000'000ull) {
                last_paint = now;
                for (auto& f : frames) f->render();
                wl_display_flush(d.display);
            }
            if (!display_wait(d, 100)) break;
        }

        supervisor.shutdown();
        display_disconnect(d);
        std::printf("otakushell preview: bye\n");
        return 0;
    }

    if (cmd == "settings") {
        return otaku::run_settings();
    }

    if (cmd == "lock" || cmd == "unlock") {
        return do_lock();
    }

    if (cmd == "reload") {
        return do_reload();
    }

    if (cmd == "status") {
        void* region = shm_open_region("status", kStatusRegionSize,
                                       /*create=*/false);
        if (!region) {
            std::printf("otakushell: otakud is not running.\n");
            return 1;
        }
        const StatusRegion* st = static_cast<const StatusRegion*>(region);
        if (st->magic.load(std::memory_order_relaxed) != kStatusMagic) {
            std::printf("otakushell: no daemon status available (stale region).\n");
            shm_close_region(region, kStatusRegionSize, "status", /*unlink=*/false);
            return 1;
        }
        std::printf("otakud status\n");
        std::printf("  version : %s\n", st->version);
        std::printf("  frames  : %u\n", st->frame_count);
        const char* lock_str = st->lock_state == kLockActive   ? "locked"
                               : st->lock_state == kLockPending ? "pending"
                                                                : "off";
        std::printf("  lock    : %s\n", lock_str);
        const uint32_t n = st->module_count.load(std::memory_order_relaxed);
        std::printf("  modules : %u\n", n);
        for (uint32_t i = 0; i < n; ++i)
            std::printf("    - %-16s pid %d\n", st->modules[i].name,
                        st->modules[i].pid);
        shm_close_region(region, kStatusRegionSize, "status", /*unlink=*/false);
        return 0;
    }

    if (cmd == "module") {
        if (argc < 4) {
            usage();
            return 1;
        }
        const std::string action = argv[2];
        const std::string name = argv[3];
        const bool disable = action == "disable";
        const bool enable = action == "enable";
        if (!enable && !disable) {
            std::fprintf(stderr, "otakushell: expected 'enable' or 'disable'\n");
            return 1;
        }

        std::string cfg_path = getenv("OTAKU_CONFIG") ? getenv("OTAKU_CONFIG")
                                                      : "config.toml";
        ShellConfig cfg;
        if (!load_config(cfg_path, cfg)) {
            std::fprintf(stderr, "otakushell: could not load '%s'\n", cfg_path.c_str());
            return 1;
        }

        bool known = cfg.modules.count(name) > 0;
        for (const auto& f : cfg.frames) {
            for (const auto& m : f.order)
                if (m == name) known = true;
        }
        if (!known) {
            std::fprintf(stderr, "otakushell: unknown module '%s'\n", name.c_str());
            return 1;
        }

        if (!set_module_disabled(name, disable)) {
            std::fprintf(stderr, "otakushell: could not update module state file\n");
            return 1;
        }
        std::printf("otakushell: module '%s' %s\n", name.c_str(),
                    disable ? "disabled" : "enabled");
        // Apply the change live if the daemon is running.
        int pid = 0;
        if (read_pidfile(pid)) {
            kill(pid, SIGUSR1);
            std::printf("otakushell: signaled otakud (pid %d) to reload\n", pid);
        }
        return 0;
    }

    if (cmd == "exec") {
        if (argc < 3) {
            std::fprintf(stderr, "otakushell: exec needs a command\n");
            return 1;
        }
        std::string joined;
        for (int i = 2; i < argc; ++i) {
            if (i > 2) joined += ' ';
            joined += argv[i];
        }
        if (hyprland_available()) {
            // Escape single quotes for the shell wrapper.
            std::string esc;
            for (char c : joined) {
                if (c == '\'') esc += "'\\''";
                else esc += c;
            }
            std::string hypr = "hyprctl dispatch exec '" + esc + "' 2>/dev/null";
            int rc = std::system(hypr.c_str());
            if (rc == 0) return 0;
        }
        int rc = std::system(joined.c_str());
        return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
    }

    usage();
    return 1;
}