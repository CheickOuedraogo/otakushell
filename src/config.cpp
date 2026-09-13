#include "otaku/config.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include <unistd.h>

namespace otaku {

std::optional<Anchor> parse_anchor(const std::string& s) {
    if (s == "top") return Anchor::Top;
    if (s == "bottom") return Anchor::Bottom;
    if (s == "left") return Anchor::Left;
    if (s == "right") return Anchor::Right;
    if (s == "full") return Anchor::Full;
    if (s == "none") return Anchor::None;
    return std::nullopt;
}

namespace {

Color to_color(const std::string& hex, float alpha = 1.0f) {
    Color c{0, 0, 0, alpha};
    if (hex.size() >= 7 && hex[0] == '#') {
        auto nib = [](char h) -> int {
            if (h >= '0' && h <= '9') return h - '0';
            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
            if (h >= 'A' && h <= 'F') return h - 'A' + 10;
            return 0;
        };
        auto byte = [&](int i) { return (nib(hex[i]) << 4) | nib(hex[i + 1]); };
        c.r = byte(1) / 255.0f;
        c.g = byte(3) / 255.0f;
        c.b = byte(5) / 255.0f;
    }
    return c;
}

std::string to_hex(const Color& c) {
    auto byte = [](float v) -> int {
        const int b = static_cast<int>(v * 255.0f + 0.5f);
        return std::clamp(b, 0, 255);
    };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", byte(c.r), byte(c.g),
                  byte(c.b));
    return buf;
}

// Serialize an option value back as the typed TOML scalar it was loaded from
// (bool / int / string). Prevents "interval_ms = 2000" from regressing into a
// quoted string on every save.
void insert_option(toml::table& mo, const std::string& key, const std::string& v) {
    std::string lo = v;
    for (auto& ch : lo) ch = static_cast<char>(std::tolower(ch));
    if (lo == "true" || lo == "false") {
        mo.insert(key, toml::value<bool>(lo == "true"));
        return;
    }
    errno = 0;
    char* end = nullptr;
    const long long i = std::strtoll(v.c_str(), &end, 10);
    if (errno == 0 && end && *end == '\0' && end != v.c_str()) {
        mo.insert(key, toml::value<int64_t>(i));
        return;
    }
    mo.insert(key, toml::value<std::string>(v));
}

const char* anchor_str(Anchor a) {
    switch (a) {
        case Anchor::Top: return "top";
        case Anchor::Bottom: return "bottom";
        case Anchor::Left: return "left";
        case Anchor::Right: return "right";
        case Anchor::Full: return "full";
        case Anchor::None: return "none";
    }
    return "none";
}

}  // namespace

bool load_config(const std::string& path, ShellConfig& out) {
    toml::table tbl;
    try {
        tbl = toml::parse_file(path);
    } catch (const toml::parse_error&) {
        return false;
    }

    if (auto sh = tbl["shell"].as_table()) {
        if (auto m = (*sh)["monitor"].value<std::string>()) out.monitor = *m;
        if (auto h = (*sh)["height"].value<int>()) out.height = *h;
        if (auto f = (*sh)["font"].value<std::string>()) out.font = *f;
        if (auto t = (*sh)["theme"].value<std::string>()) out.theme = *t;
        if (auto v = (*sh)["take_over"].value<bool>()) out.take_over = *v;
        if (auto v = (*sh)["take_over_dry_run"].value<bool>()) out.take_over_dry_run = *v;
        if (auto a = (*sh)["take_over_kill"].as_array()) {
            out.take_over_kill.clear();
            for (const auto& m : *a)
                if (auto s = m.value<std::string>()) out.take_over_kill.push_back(*s);
        }
    }

    if (auto th = tbl["theme"].as_table()) {
        if (auto s = (*th)["colors"].as_table()) {
            if (auto v = (*s)["background"].value<std::string>())
                out.colors.background = to_color(*v);
            if (auto v = (*s)["foreground"].value<std::string>())
                out.colors.foreground = to_color(*v);
            if (auto v = (*s)["accent"].value<std::string>())
                out.colors.accent = to_color(*v);
            if (auto v = (*s)["muted"].value<std::string>())
                out.colors.muted = to_color(*v);
        }
    }

    out.frames.clear();
    if (auto fr = tbl["frame"].as_array()) {
        for (const auto& f : *fr) {
            auto* t = f.as_table();
            if (!t) continue;
            FrameSpec spec;
            if (auto v = (*t)["id"].value<std::string>()) spec.id = *v;
            if (auto v = (*t)["anchor"].value<std::string>())
                if (auto a = parse_anchor(*v)) spec.anchor = *a;
            if (auto v = (*t)["exclusive_zone"].value<bool>()) spec.exclusive = *v;
            if (auto v = (*t)["hidden"].value<std::string>()) spec.hidden = (*v == "auto");
            if (auto v = (*t)["lock"].value<bool>()) spec.lock = *v;
            if (auto v = (*t)["order"].as_array()) {
                for (const auto& m : *v)
                    if (auto s = m.value<std::string>()) spec.order.push_back(*s);
            }
            out.frames.push_back(std::move(spec));
        }
    }

    out.modules.clear();
    if (auto ms = tbl["modules"].as_table()) {
        for (const auto& [mod_name, mod_node] : *ms) {
            auto* opts = mod_node.as_table();
            if (!opts) continue;
            ModuleOptions mopts;
            for (const auto& [key, val] : *opts) {
                // Check scalar types explicitly: `value<bool>()` silently
                // converts any non-zero integer to true, so never rely on
                // implicit conversions when round-tripping module options.
                if (val.is_boolean())
                    mopts[std::string(key)] =
                        val.as_boolean()->get() ? std::string("true")
                                                : std::string("false");
                else if (val.is_integer())
                    mopts[std::string(key)] =
                        std::to_string(val.as_integer()->get());
                else if (auto s = val.value<std::string>())
                    mopts[std::string(key)] = *s;
            }
            out.modules[std::string(mod_name)] = std::move(mopts);
        }
    }

    // Apply the `otakushell module enable|disable` state file.
    read_disabled_modules(out.disabled);
    apply_module_overrides(out);
    return true;
}

std::string serialize_module_options(const ModuleOptions& m) {
    std::string out;
    for (const auto& [k, v] : m) {
        if (!out.empty()) out += '&';
        out += k;
        out += '=';
        out += v;
    }
    return out;
}

std::string module_state_path() {
    const char* xdg = getenv("XDG_STATE_HOME");
    const char* home = getenv("HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else if (home && *home) {
        base = std::string(home) + "/.local/state";
    } else {
        base = "/tmp";
    }
    return base + "/otakushell/modules.state";
}

bool read_disabled_modules(std::set<std::string>& out) {
    out.clear();
    toml::table tbl;
    try {
        tbl = toml::parse_file(module_state_path());
    } catch (const toml::parse_error&) {
        return false;  // no state yet → nothing disabled
    }
    if (auto dis = tbl["disabled"].as_array()) {
        for (const auto& m : *dis)
            if (auto s = m.value<std::string>()) out.insert(*s);
    }
    return true;
}

bool write_disabled_modules(const std::set<std::string>& disabled) {
    const std::string path = module_state_path();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    if (ec) return false;

    toml::table tbl;
    toml::array arr;
    for (const auto& d : disabled) arr.push_back(d);
    tbl.insert("disabled", std::move(arr));

    // Atomic write: temp file + rename.
    const std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << tbl << "\n";
    f.close();
    if (!f) return false;
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool set_module_disabled(const std::string& name, bool disable) {
    std::set<std::string> current;
    read_disabled_modules(current);
    if (disable) {
        current.insert(name);
    } else {
        current.erase(name);
    }
    return write_disabled_modules(current);
}

void apply_module_overrides(ShellConfig& cfg) {
    for (auto& frame : cfg.frames) {
        auto& order = frame.order;
        order.erase(std::remove_if(order.begin(), order.end(),
                                   [&](const std::string& m) {
                                       return cfg.disabled.count(m) > 0;
                                   }),
                    order.end());
    }
}

bool save_config(const std::string& path, const ShellConfig& cfg) {
    toml::table shell;
    shell.insert("monitor", toml::value<std::string>(cfg.monitor));
    shell.insert("height", toml::value<int64_t>(cfg.height));
    shell.insert("font", toml::value<std::string>(cfg.font));
    shell.insert("theme", toml::value<std::string>(cfg.theme));
    shell.insert("take_over", toml::value<bool>(cfg.take_over));
    shell.insert("take_over_dry_run", toml::value<bool>(cfg.take_over_dry_run));
    toml::array kill;
    for (const auto& k : cfg.take_over_kill) kill.push_back(k);
    shell.insert("take_over_kill", std::move(kill));

    toml::table colors;
    colors.insert("background", toml::value<std::string>(to_hex(cfg.colors.background)));
    colors.insert("foreground", toml::value<std::string>(to_hex(cfg.colors.foreground)));
    colors.insert("accent", toml::value<std::string>(to_hex(cfg.colors.accent)));
    colors.insert("muted", toml::value<std::string>(to_hex(cfg.colors.muted)));
    toml::table theme;
    theme.insert("colors", std::move(colors));

    toml::array frames;
    for (const auto& f : cfg.frames) {
        toml::table ft;
        ft.insert("id", toml::value<std::string>(f.id));
        ft.insert("anchor", toml::value<std::string>(anchor_str(f.anchor)));
        if (f.exclusive) ft.insert("exclusive_zone", toml::value<bool>(true));
        if (f.hidden) ft.insert("hidden", toml::value<std::string>("auto"));
        if (f.lock) ft.insert("lock", toml::value<bool>(true));
        toml::array order;
        for (const auto& m : f.order) order.push_back(m);
        ft.insert("order", std::move(order));
        frames.push_back(std::move(ft));
    }

    toml::table mods;
    for (const auto& [name, opts] : cfg.modules) {
        toml::table mo;
        for (const auto& [k, v] : opts) insert_option(mo, k, v);
        mods.insert(name, std::move(mo));
    }

    toml::table root;
    root.insert("shell", std::move(shell));
    root.insert("theme", std::move(theme));
    root.insert("frame", std::move(frames));
    root.insert("modules", std::move(mods));

    // Atomic write (temp file + rename) so a crash never leaves a partial
    // config behind. Note: re-serializing loses the original inline comments;
    // the settings UI takes over as the visual editor.
    const std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << root << "\n";
    f.close();
    if (!f) return false;
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace otaku