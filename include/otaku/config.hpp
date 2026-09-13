#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace otaku {

enum class Anchor { None, Top, Bottom, Left, Right, Full };

struct Color {
    float r{0}, g{0}, b{0}, a{1.0f};
};

struct ThemeColors {
    Color background{0.117f, 0.117f, 0.180f, 1.0f};  // #1e1e2e
    Color foreground{0.804f, 0.839f, 0.957f, 1.0f};  // #cdd6f4
    Color accent{0.537f, 0.706f, 0.980f, 1.0f};      // #89b4fa
    Color muted{0.424f, 0.439f, 0.525f, 1.0f};       // #6c7086
};

struct FrameSpec {
    std::string id;
    Anchor anchor{Anchor::None};
    bool exclusive{false};
    bool hidden{false};
    bool lock{false};
    std::vector<std::string> order;
};

// Per-module options, as declared under `[modules.<name>]` in config.toml.
// They are passed verbatim to the module producer (`otakud-mod`).
using ModuleOptions = std::map<std::string, std::string>;

struct ShellConfig {
    std::string monitor{"auto"};
    int height{34};
    std::string font{"Sans 11"};
    std::string theme{"dark"};
    ThemeColors colors;
    std::vector<FrameSpec> frames;
    std::map<std::string, ModuleOptions> modules;

    // Modules disabled via `otakushell module disable` (state file override).
    std::set<std::string> disabled;

    // Take-over of an existing shell running on the desktop.
    bool take_over{true};                         // disable an existing shell at startup
    bool take_over_dry_run{false};                // only report, don't terminate
    std::vector<std::string> take_over_kill;      // extra processes to disable
};

// Load configuration from a TOML file. Returns false on parse error.
// Also applies the enable/disable state file (load_module_overrides).
bool load_config(const std::string& path, ShellConfig& out);

// Parse an anchor string ("top", "bottom", ...) into an Anchor.
std::optional<Anchor> parse_anchor(const std::string& s);

// Serialize module options to a "k=v&k=v" producer string.
std::string serialize_module_options(const ModuleOptions& m);

// -- Module enable/disable state (persisted override, not config.toml) ------

// Path of the module state file (<state>/otakushell/modules.state).
std::string module_state_path();

// Read the currently disabled-module set from the state file.
bool read_disabled_modules(std::set<std::string>& out);

// Rewrite the whole disabled set (e.g. after `module enable/disable`).
bool write_disabled_modules(const std::set<std::string>& disabled);

// Enable (disable=false) or disable (disable=true) a module persistently.
bool set_module_disabled(const std::string& name, bool disable);

// Remove the disabled modules from every frame's `order`.
void apply_module_overrides(ShellConfig& cfg);

}  // namespace otaku