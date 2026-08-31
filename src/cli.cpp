#include <cstdio>
#include <cstring>
#include <string>

#include "otaku/config.hpp"

using namespace otaku;

namespace {
void usage() {
    std::printf(
        "otakushell - otakuShell control CLI\n"
        "\n"
        "usage:\n"
        "  otakushell preview              open a window previewing the layout\n"
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
        std::printf("otakushell preview (%s): %zu frame(s) to render\n",
                    OTAKU_VERSION, cfg.frames.size());
        std::printf("otakushell: window rendering arrives in step 2.\n");
        return 0;
    }

    if (cmd == "reload" || cmd == "status" || cmd == "exec" || cmd == "module") {
        std::printf("otakushell: '%s' is not implemented yet.\n", cmd.c_str());
        return 0;
    }

    usage();
    return 1;
}
