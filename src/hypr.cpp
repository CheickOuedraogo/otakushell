#include "otaku/hypr.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

namespace otaku {

namespace {

std::string run_capture(const char* cmd) {
    std::string out;
    FILE* f = popen(cmd, "r");
    if (!f) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    return out;
}

bool process_running(const std::string& proc) {
    std::string cmd = "pgrep -x " + proc + " >/dev/null 2>&1";
    return system(cmd.c_str()) == 0;
}

}  // namespace

bool hyprland_available() {
    static const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig) return false;
    std::string state;
    state = run_capture("hyprctl -j activeworkspace 2>/dev/null");
    return !state.empty();
}

MaimReport maim_existing_shell(const std::vector<std::string>& process_kill_list,
                               bool dry_run) {
    MaimReport report;

    // Built-in list of common shell/bar/panel processes we can take over.
    static const char* builtin[] = {"waybar",    "eww",   "polybar",
                                    "cairo-dock", "matugen", "somebar"};
    std::vector<std::string> targets = process_kill_list;
    if (targets.empty()) {
        for (const auto& b : builtin) targets.push_back(b);
    }

    for (const auto& t : targets) {
        if (process_running(t)) {
            report.removed.push_back(t);
            report.replaced_anything = true;
            if (!dry_run) {
                std::string cmd = "pkill -TERM -x " + t + " 2>/dev/null";
                system(cmd.c_str());
                std::fprintf(stderr, "otakud: disabled existing shell '%s'\n",
                             t.c_str());
            } else {
                std::fprintf(stderr, "otakud [dry-run]: would disable '%s'\n",
                             t.c_str());
            }
        }
    }
    return report;
}

}  // namespace otaku
