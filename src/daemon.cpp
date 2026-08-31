#include <cstdio>
#include <string>

#include "otaku/config.hpp"

using namespace otaku;

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // Default config lookup: $OTAKU_CONFIG or ./config.toml
    std::string cfg_path = getenv("OTAKU_CONFIG") ? getenv("OTAKU_CONFIG")
                                                  : "config.toml";

    ShellConfig cfg;
    if (!load_config(cfg_path, cfg)) {
        std::fprintf(stderr, "otakud: could not load config from '%s'\n",
                     cfg_path.c_str());
        return 1;
    }

    std::printf("otakud (%s): loaded %zu frame(s)\n", OTAKU_VERSION,
                cfg.frames.size());
    for (const auto& f : cfg.frames) {
        std::printf("  - frame '%s' anchor=%d modules=%zu\n", f.id.c_str(),
                    static_cast<int>(f.anchor), f.order.size());
    }
    std::printf("otakud: Wayland integration arrives in step 1.\n");
    return 0;
}
