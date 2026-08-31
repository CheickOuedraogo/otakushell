#pragma once

#include <string>
#include <vector>

namespace otaku {

// Result of the "maim" (take over) check that runs at daemon startup.
struct MaimReport {
    bool replaced_anything{false};
    std::vector<std::string> removed;
};

// Detect and, if configured, terminate a shell already occupying the desktop
// (e.g. a running bar/panel) before otakud anchors its own layers.
//
// On Hyprland this first lists active layer-shell surfaces via `hyprctl
// layers`, then terminates any known shell binaries that are running (waybar,
// eww, polybar, etc.). `process_kill_list` is taken from config; passing an
// empty list keeps the default built-in set.
MaimReport maim_existing_shell(const std::vector<std::string>& process_kill_list,
                               bool dry_run);

// Whether the Hyprland compositor is currently available.
bool hyprland_available();

}  // namespace otaku
