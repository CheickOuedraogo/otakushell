#include "otaku/config.hpp"

#include <toml++/toml.hpp>

#include <string>

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

    return true;
}

}  // namespace otaku
