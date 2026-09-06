#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "otaku/config.hpp"
#include "otaku/frame.hpp"
#include "otaku/wayland.hpp"

using namespace otaku;

namespace {
void usage() {
    std::printf(
        "otakushell - otakuShell control CLI\n"
        "\n"
        "usage:\n"
        "  otakushell preview [frame-id]    open a window previewing a frame\n"
        "  otakushell reload               hot-reload config\n"
        "  otakushell status               show active frames/modules\n"
        "  otakushell module enable|disable <name>\n"
        "  otakushell exec <cmd>\n");
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

        // One preview window per frame; default to the top navbar.
        const std::string wanted = (argc >= 3) ? argv[2] : "navbar-top";
        const FrameSpec* fspec = nullptr;
        for (const auto& f : cfg.frames) {
            if (f.id == wanted) {
                fspec = &f;
                break;
            }
        }
        if (!fspec) {
            std::fprintf(stderr, "otakushell: unknown frame '%s' (see config.toml)\n",
                         wanted.c_str());
            return 1;
        }
        if (fspec->anchor == Anchor::Full || fspec->anchor == Anchor::None) {
            std::fprintf(stderr,
                         "otakushell: frame '%s' cannot be previewed in a window "
                         "(anchors full/none are for the desktop)\n",
                         fspec->id.c_str());
            return 1;
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

        // Bar frames span a mock screen; edges run along it.
        constexpr int32_t kMockScreen = 1280;
        const bool horizontal = (fspec->anchor == Anchor::Top ||
                                 fspec->anchor == Anchor::Bottom);
        const int32_t win_w = horizontal ? kMockScreen : cfg.height;
        const int32_t win_h = horizontal ? cfg.height : kMockScreen;

        auto surf = create_toplevel_surface(
            d, "otakushell preview \xe2\x80\x94 " + fspec->id, win_w, win_h, &alive);
        if (!surf) {
            std::fprintf(stderr, "otakushell: failed to create preview window\n");
            display_disconnect(d);
            return 1;
        }

        Frame frame(*fspec);
        frame.set_surface(std::move(surf));

        // Let the compositor deliver the initial configure, then first frame.
        wl_display_roundtrip(d.display);
        frame.render();
        wl_display_flush(d.display);

        std::printf("otakushell preview (%s): frame '%s' (%s)\n", OTAKU_VERSION,
                    fspec->id.c_str(),
                    horizontal ? "top/bottom bar" : "side edge");
        std::printf("otakushell: close the window to quit.\n");

        while (alive.load()) {
            if (!display_wait(d, 100)) break;
        }

        display_disconnect(d);
        std::printf("otakushell preview: bye\n");
        return 0;
    }

    if (cmd == "reload" || cmd == "status" || cmd == "exec" || cmd == "module") {
        std::printf("otakushell: '%s' is not implemented yet.\n", cmd.c_str());
        return 0;
    }

    usage();
    return 1;
}
