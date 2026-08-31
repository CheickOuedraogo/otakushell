#pragma once

#include <string>
#include <vector>
#include <optional>

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

struct ShellConfig {
    std::string monitor{"auto"};
    int height{34};
    std::string font{"Sans 11"};
    std::string theme{"dark"};
    ThemeColors colors;
    std::vector<FrameSpec> frames;
};

// Load configuration from a TOML file. Returns false on parse error.
bool load_config(const std::string& path, ShellConfig& out);

// Parse an anchor string ("top", "bottom", ...) into an Anchor.
std::optional<Anchor> parse_anchor(const std::string& s);

}  // namespace otaku
