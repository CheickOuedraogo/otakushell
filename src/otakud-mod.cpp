// otakud-mod — module producer(s) for otakuShell.
//
// Runs as a separate process per module type and publishes one formatted
// display string per interval into a lock-free shared-memory ring
// (`/otakuShell-<user>-mod-<name>`). The shell (otakud / preview) only reads.
//
// Data sources are existing permissively-licensed tools/no syscalls, never
// re-implemented here:
//   - audio      : wpctl (WirePlumber, Apache-2.0)
//   - brightness : brightnessctl (MIT)
//   - workspace  : hyprctl (Hyprland, BSD-3-Clause)
//   - cpu/mem    : /proc (parsing style from yambar cpu.c, MIT)
//   - clock      : strftime(3)
//
// Usage:
//   otakud-mod <name> [opts] [--simulate] [--once] [--daemonize]
//   opts is "k=v&k=v" (parsed from [modules.<name>] in config.toml).

#include <csignal>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include <algorithm>
#include <cmath>
#include <sys/types.h>
#include <unistd.h>

#if __has_include(<dbus/dbus.h>)
#include <dbus/dbus.h>
#define HAVE_DBUS 1
#endif

#include "otaku/shm.hpp"

using namespace otaku;

namespace {

volatile std::sig_atomic_t g_running = 1;
void on_signal(int) { g_running = 0; }

// --- tiny options ----------------------------------------------------------

struct Opts {
    std::map<std::string, std::string> kv;

    std::string get(const char* key, const char* def) const {
        auto it = kv.find(key);
        return it == kv.end() ? def : it->second;
    }

    int get_int(const char* key, int def) const {
        auto it = kv.find(key);
        return it == kv.end() ? def : std::atoi(it->second.c_str());
    }
};

Opts parse_opts(const std::string& s) {
    Opts o;
    size_t start = 0;
    while (start < s.size()) {
        size_t amp = s.find('&', start);
        if (amp == std::string::npos) amp = s.size();
        std::string pair = s.substr(start, amp - start);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && eq > 0)
            o.kv[pair.substr(0, eq)] = pair.substr(eq + 1);
        start = amp + 1;
    }
    return o;
}

// --- helpers ---------------------------------------------------------------

std::string run_capture(const std::string& cmd) {
    std::string out;
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    return out;
}

std::string rtrim(const std::string& s) {
    size_t end = s.find_last_not_of(" \t\r\n");
    return end == std::string::npos ? "" : s.substr(0, end + 1);
}

std::string replace_all(std::string s, const std::string& what,
                        const std::string& with) {
    size_t at = 0;
    while ((at = s.find(what, at)) != std::string::npos) {
        s.replace(at, what.size(), with);
        at += with.size();
    }
    return s;
}

// --- mini JSON parser (enough for hyprctl output) --------------------------

namespace jmin {

struct Value {
    enum class T { Null, Bool, Int, Double, Str, Arr, Obj } t{T::Null};
    bool b{false};
    int64_t i{0};
    double d{0};
    std::string s;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    const Value* get(const std::string& key) const {
        for (const auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    bool parse(Value& out) {
        skip_ws();
        return parse_value(out);
    }

private:
    void skip_ws() {
        while (p_ < s_.size() &&
               (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r'))
            ++p_;
    }
    bool peek(char c) { skip_ws(); return p_ < s_.size() && s_[p_] == c; }
    bool eat(char c) {
        skip_ws();
        if (p_ >= s_.size() || s_[p_] != c) return false;
        ++p_;
        return true;
    }

    bool parse_value(Value& out) {
        skip_ws();
        if (p_ >= s_.size()) return false;
        const char c = s_[p_];
        if (c == '{') return parse_obj(out);
        if (c == '[') return parse_arr(out);
        if (c == '"') return parse_string(out);
        if (c == 't') return parse_lit("true", out, Value::T::Bool, true);
        if (c == 'f') return parse_lit("false", out, Value::T::Bool, false);
        if (c == 'n') return parse_lit("null", out, Value::T::Null, false);
        return parse_number(out);
    }

    bool parse_lit(const char* word, Value& out, Value::T t, bool bval) {
        size_t len = std::strlen(word);
        if (s_.compare(p_, len, word) != 0) return false;
        p_ += len;
        out.t = t;
        out.b = bval;
        return true;
    }

    bool parse_string(Value& out) {
        if (!eat('"')) return false;
        std::string str;
        while (p_ < s_.size() && s_[p_] != '"') {
            if (s_[p_] == '\\' && p_ + 1 < s_.size()) {
                ++p_;
                switch (s_[p_]) {
                    case 'n': str += '\n'; break;
                    case 't': str += '\t'; break;
                    case 'r': str += '\r'; break;
                    default:  str += s_[p_]; break;
                }
                ++p_;
            } else {
                str += s_[p_++];
            }
        }
        if (!eat('"')) return false;
        out.t = Value::T::Str;
        out.s = str;
        return true;
    }

    bool parse_number(Value& out) {
        size_t start = p_;
        bool is_double = false;
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (c == '-' || c == '+' || (c >= '0' && c <= '9') || c == '.' ||
                c == 'e' || c == 'E') {
                if (c == '.' || c == 'e' || c == 'E') is_double = true;
                ++p_;
            } else {
                break;
            }
        }
        if (p_ == start) return false;
        const std::string num(s_.substr(start, p_ - start));
        if (is_double) {
            out.t = Value::T::Double;
            out.d = std::strtod(num.c_str(), nullptr);
        } else {
            out.t = Value::T::Int;
            out.i = std::strtoll(num.c_str(), nullptr, 10);
        }
        return true;
    }

    bool parse_arr(Value& out) {
        if (!eat('[')) return false;
        out.t = Value::T::Arr;
        skip_ws();
        if (peek(']')) return eat(']');
        while (true) {
            Value v;
            if (!parse_value(v)) return false;
            out.arr.push_back(std::move(v));
            if (eat(']')) return true;
            if (!eat(',')) return false;
        }
    }

    bool parse_obj(Value& out) {
        if (!eat('{')) return false;
        out.t = Value::T::Obj;
        skip_ws();
        if (peek('}')) return eat('}');
        while (true) {
            skip_ws();
            Value key;
            if (!parse_string(key)) return false;
            if (!eat(':')) return false;
            Value v;
            if (!parse_value(v)) return false;
            out.obj.emplace_back(key.s, std::move(v));
            if (eat('}')) return true;
            if (!eat(',')) return false;
        }
    }

    std::string_view s_;
    size_t p_{0};
};

bool parse(std::string_view text, Value& out) {
    Parser p(text);
    return p.parse(out);
}

}  // namespace jmin

// --- providers -------------------------------------------------------------

std::string provider_clock(const Opts& o, bool simulate) {
    (void)simulate;
    time_t t = time(nullptr);
    struct tm now;
    localtime_r(&t, &now);
    const std::string fmt = o.get("format", "%H:%M");
    const std::string datef = o.get("date_format", "%a %d %b");
    char buf[128];
    std::strftime(buf, sizeof(buf), (datef + "  " + fmt).c_str(), &now);
    return buf;
}

// CPU % via /proc/stat deltas (parsing approach from yambar's cpu.c).
// Keeps a static state; only meaningful when called at a steady interval.
static uint64_t prev_total = 0;
static uint64_t prev_idle = 0;

int cpu_percent() {
    FILE* f = std::fopen("/proc/stat", "re");
    if (!f) return -1;
    char line[512];
    uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0,
             irq = 0, softirq = 0, steal = 0;
    bool got = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::sscanf(line,
                        "cpu %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                        " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
                        &user, &nice, &sys, &idle, &iowait, &irq, &softirq,
                        &steal) ==
            8) {
            got = true;
            break;
        }
    }
    std::fclose(f);
    if (!got) return -1;

    const uint64_t total = user + nice + sys + idle + iowait + irq + softirq + steal;
    const uint64_t idl = idle + iowait;
    if (prev_total == 0) {
        prev_total = total;
        prev_idle = idl;
        return 0;
    }
    const uint64_t dt = total - prev_total;
    const uint64_t di = idl - prev_idle;
    prev_total = total;
    prev_idle = idl;
    if (dt == 0) return 0;
    const double used = static_cast<double>(dt - di) / static_cast<double>(dt);
    return static_cast<int>(std::lround(used * 100.0));
}

std::string provider_sysinfo(const Opts& o, bool simulate) {
    const std::string fmt = o.get("format", "CPU {cpu}%  MEM {mem}");
    int cpu = cpu_percent();
    if (simulate) {
        static int sim_cpu = 23;
        sim_cpu = 15 + (sim_cpu * 7 % 35);  // pseudo-random but stable-ish
        cpu = sim_cpu;
    }
    if (cpu < 0) cpu = 0;

    std::string mem;
    FILE* f = std::fopen("/proc/meminfo", "re");
    if (f) {
        char line[256];
        uint64_t total = 0, avail = 0;
        while (std::fgets(line, sizeof(line), f)) {
            uint64_t v = 0;
            if (std::sscanf(line, "MemTotal: %" SCNu64, &v) == 1) total = v;
            else if (std::sscanf(line, "MemAvailable: %" SCNu64, &v) == 1) {
                avail = v;
                break;
            }
        }
        std::fclose(f);
        if (total > 0 && avail > 0) {
            const double total_g = static_cast<double>(total) / (1024.0 * 1024.0);
            const double used_g = static_cast<double>(total - avail) / (1024.0 * 1024.0);
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%.1fG/%.1fG", used_g, total_g);
            mem = buf;
        }
    }
    if (mem.empty()) {
        if (simulate) {
            mem = "4.2G/15.6G";
        } else {
            mem = "--";
        }
    }

    std::string out = replace_all(fmt, "{cpu}", std::to_string(cpu));
    out = replace_all(out, "{mem}", mem);
    return out;
}

std::string provider_audio(const Opts& o, bool simulate) {
    if (simulate) return "VOL 62%";
    const std::string vol = run_capture("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
    const std::string muted = run_capture("wpctl get-mute @DEFAULT_AUDIO_SINK@ 2>/dev/null");
    if (vol.empty() || vol.find("Volume:") == std::string::npos)
        return "VOL --";
    double value = std::strtod(vol.c_str() + 7, nullptr);
    const bool is_muted = muted.find("Muted") != std::string::npos ||
                          vol.find("MUTED") != std::string::npos;
    int pct = static_cast<int>(std::lround(value * 100.0));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return is_muted ? std::string("MUTE") : "VOL " + std::to_string(pct) + "%";
}

std::string provider_brightness(const Opts& o, bool simulate) {
    (void)o;
    if (simulate) return "BRI 70%";
    const std::string out = run_capture("brightnessctl -m 2>/dev/null");
    const size_t nl = out.find('\n');
    const std::string line = rtrim(nl == std::string::npos ? out : out.substr(0, nl));
    size_t comma1 = line.find(',');
    size_t comma2 = comma1 == std::string::npos ? std::string::npos : line.find(',', comma1 + 1);
    size_t comma3 = comma2 == std::string::npos ? std::string::npos : line.find(',', comma2 + 1);
    if (comma3 == std::string::npos) return "BRI --";
    const int pct = std::atoi(line.c_str() + comma3 + 1);
    return "BRI " + std::to_string(pct) + "%";
}

std::string to_lower_str(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            // trim
            size_t a = cur.find_first_not_of(" \t");
            size_t b = cur.find_last_not_of(" \t");
            if (a != std::string::npos) cur = cur.substr(a, b - a + 1);
            else cur.clear();
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    size_t a = cur.find_first_not_of(" \t");
    size_t b = cur.find_last_not_of(" \t");
    if (a != std::string::npos) cur = cur.substr(a, b - a + 1);
    else cur.clear();
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// --- D-Bus helpers (libdbus) ------------------------------------------------
#ifdef HAVE_DBUS
namespace dbus_help {
inline DBusMessage* prop_get(DBusConnection* conn, const char* service,
                             const char* path, const char* iface,
                             const char* prop) {
    DBusMessage* msg = dbus_message_new_method_call(service, path,
        "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return nullptr;
    const char* i = iface;
    const char* p = prop;
    DBusMessageIter it, sub;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &i);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &p);
    DBusError err; dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1500, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err)) dbus_error_free(&err);
    return reply; // caller must unref
}
inline bool variant_get_string(DBusMessage* reply, std::string& out) {
    if (!reply) return false;
    DBusMessageIter it, var;
    if (!dbus_message_iter_init(reply, &it)) return false;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT) return false;
    dbus_message_iter_recurse(&it, &var);
    if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING) return false;
    const char* s = nullptr;
    dbus_message_iter_get_basic(&var, &s);
    if (s) out = s;
    return s != nullptr;
}
inline bool variant_get_uint32(DBusMessage* reply, uint32_t& out) {
    if (!reply) return false;
    DBusMessageIter it, var;
    if (!dbus_message_iter_init(reply, &it)) return false;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT) return false;
    dbus_message_iter_recurse(&it, &var);
    int t = dbus_message_iter_get_arg_type(&var);
    if (t == DBUS_TYPE_UINT32) { dbus_message_iter_get_basic(&var, &out); return true; }
    if (t == DBUS_TYPE_BYTE) { uint8_t b=0; dbus_message_iter_get_basic(&var,&b); out=b; return true; }
    return false;
}
inline bool variant_get_byte_array(DBusMessage* reply, std::string& out) {
    if (!reply) return false;
    DBusMessageIter it, var, arr;
    if (!dbus_message_iter_init(reply, &it)) return false;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT) return false;
    dbus_message_iter_recurse(&it, &var);
    if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_ARRAY) return false;
    dbus_message_iter_recurse(&var, &arr);
    out.clear();
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_BYTE) {
        uint8_t b=0; dbus_message_iter_get_basic(&arr,&b); out.push_back(static_cast<char>(b));
        dbus_message_iter_next(&arr);
    }
    return true;
}
inline bool variant_get_bool(DBusMessage* reply, bool& out) {
    if (!reply) return false;
    DBusMessageIter it, var;
    if (!dbus_message_iter_init(reply, &it)) return false;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT) return false;
    dbus_message_iter_recurse(&it, &var);
    if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN) return false;
    dbus_bool_t b=0; dbus_message_iter_get_basic(&var,&b); out = b!=0; return true;
}
} // namespace dbus_help
#endif

std::string provider_wifi_dbus_try() {
#ifdef HAVE_DBUS
    DBusError err; dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn || dbus_error_is_set(&err)) { if (dbus_error_is_set(&err)) dbus_error_free(&err); return ""; }
    // GetDevices
    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager","org.freedesktop.NetworkManager","GetDevices");
    if (!msg) return "";
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1500, &err);
    dbus_message_unref(msg);
    if (!reply || dbus_error_is_set(&err)) { if(reply) dbus_message_unref(reply); if(dbus_error_is_set(&err)) dbus_error_free(&err); return ""; }
    DBusMessageIter it, arr;
    dbus_message_iter_init(reply, &it);
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) { dbus_message_unref(reply); return ""; }
    dbus_message_iter_recurse(&it, &arr);
    std::string best;
    uint32_t best_strength = 0;
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_OBJECT_PATH) {
        const char* dev_path=nullptr; dbus_message_iter_get_basic(&arr,&dev_path);
        if (dev_path) {
            DBusMessage* rtype = dbus_help::prop_get(conn, "org.freedesktop.NetworkManager", dev_path,
                "org.freedesktop.NetworkManager.Device", "DeviceType");
            uint32_t dtype=0; bool ok=false;
            if (rtype) { ok = dbus_help::variant_get_uint32(rtype, dtype); dbus_message_unref(rtype); }
            if (ok && dtype == 2) { // WIFI
                DBusMessage* rap = dbus_help::prop_get(conn, "org.freedesktop.NetworkManager", dev_path,
                    "org.freedesktop.NetworkManager.Device.Wireless", "ActiveAccessPoint");
                std::string ap; bool has_ap=false;
                if (rap) { has_ap = dbus_help::variant_get_string(rap, ap); dbus_message_unref(rap); }
                if (has_ap && ap != "/") {
                    std::string ssid;
                    DBusMessage* rssid = dbus_help::prop_get(conn, "org.freedesktop.NetworkManager", ap.c_str(),
                        "org.freedesktop.NetworkManager.AccessPoint", "Ssid");
                    if (rssid) { dbus_help::variant_get_byte_array(rssid, ssid); dbus_message_unref(rssid); }
                    uint32_t strength=0;
                    DBusMessage* rstr = dbus_help::prop_get(conn, "org.freedesktop.NetworkManager", ap.c_str(),
                        "org.freedesktop.NetworkManager.AccessPoint", "Strength");
                    if (rstr) { dbus_help::variant_get_uint32(rstr, strength); dbus_message_unref(rstr); }
                    if (!ssid.empty()) {
                        if (strength > best_strength) { best_strength = strength; best = ssid + " " + std::to_string(strength) + "%"; }
                        else if (best.empty()) best = ssid + " " + std::to_string(strength) + "%";
                    }
                }
            }
        }
        dbus_message_iter_next(&arr);
    }
    dbus_message_unref(reply);
    if (!best.empty()) return best;
    return "";
#else
    return "";
#endif
}

std::string provider_wifi(const Opts& o, bool simulate) {
    (void)o;
    if (simulate) return "WIFI otaku-5G 78%";
    std::string dbus = provider_wifi_dbus_try();
    if (!dbus.empty()) return "WIFI " + dbus;
    // Fallback: nmcli
    std::string out = run_capture("nmcli -t -f active,ssid,signal dev wifi 2>/dev/null | grep '^yes'");
    if (!out.empty()) {
        // yes:ssid:signal
        size_t p1 = out.find(':'); size_t p2 = p1==std::string::npos?std::string::npos:out.find(':', p1+1);
        if (p1!=std::string::npos && p2!=std::string::npos) {
            std::string ssid = out.substr(p1+1, p2-p1-1);
            std::string sig = rtrim(out.substr(p2+1));
            // strip newline
            size_t nl = sig.find('\n'); if (nl!=std::string::npos) sig = sig.substr(0,nl);
            if (!ssid.empty()) return "WIFI " + ssid + " " + sig + "%";
        }
    }
    // fallback iw
    std::string iw = run_capture("iwgetid -r 2>/dev/null");
    iw = rtrim(iw);
    if (!iw.empty()) return "WIFI " + iw;
    return "WIFI --";
}

std::string provider_bluetooth_dbus_try() {
#ifdef HAVE_DBUS
    DBusError err; dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn || dbus_error_is_set(&err)) { if(dbus_error_is_set(&err)) dbus_error_free(&err); return ""; }
    DBusMessage* msg = dbus_message_new_method_call("org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (!msg) return "";
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1500, &err);
    dbus_message_unref(msg);
    if (!reply || dbus_error_is_set(&err)) { if(reply) dbus_message_unref(reply); if(dbus_error_is_set(&err)) dbus_error_free(&err); return ""; }
    // We just count connected devices and check adapter powered
    DBusMessageIter it;
    dbus_message_iter_init(reply, &it);
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) { dbus_message_unref(reply); return ""; }
    DBusMessageIter dict;
    dbus_message_iter_recurse(&it, &dict);
    bool powered = false; int connected = 0;
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, pathIter, ifaceDict;
        const char* obj_path=nullptr;
        dbus_message_iter_recurse(&dict, &entry);
        dbus_message_iter_get_basic(&entry, &obj_path);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &ifaceDict);
        while (dbus_message_iter_get_arg_type(&ifaceDict) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ie, props;
            dbus_message_iter_recurse(&ifaceDict, &ie);
            const char* iface=nullptr; dbus_message_iter_get_basic(&ie,&iface);
            dbus_message_iter_next(&ie);
            dbus_message_iter_recurse(&ie, &props);
            if (iface && std::strcmp(iface,"org.bluez.Adapter1")==0) {
                while (dbus_message_iter_get_arg_type(&props)==DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter pe, v;
                    dbus_message_iter_recurse(&props,&pe);
                    const char* key=nullptr; dbus_message_iter_get_basic(&pe,&key);
                    dbus_message_iter_next(&pe);
                    dbus_message_iter_recurse(&pe,&v);
                    if (key && std::strcmp(key,"Powered")==0) {
                        int t = dbus_message_iter_get_arg_type(&v);
                        if (t==DBUS_TYPE_VARIANT) {
                            DBusMessageIter vv; dbus_message_iter_recurse(&v,&vv);
                            if (dbus_message_iter_get_arg_type(&vv)==DBUS_TYPE_BOOLEAN) { dbus_bool_t b; dbus_message_iter_get_basic(&vv,&b); powered=b; }
                        }
                    }
                    dbus_message_iter_next(&props);
                }
            } else if (iface && std::strcmp(iface,"org.bluez.Device1")==0) {
                bool conn=false;
                while (dbus_message_iter_get_arg_type(&props)==DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter pe, v;
                    dbus_message_iter_recurse(&props,&pe);
                    const char* key=nullptr; dbus_message_iter_get_basic(&pe,&key);
                    dbus_message_iter_next(&pe);
                    dbus_message_iter_recurse(&pe,&v);
                    if (key && std::strcmp(key,"Connected")==0) {
                        int t=dbus_message_iter_get_arg_type(&v);
                        if(t==DBUS_TYPE_VARIANT){DBusMessageIter vv; dbus_message_iter_recurse(&v,&vv); if(dbus_message_iter_get_arg_type(&vv)==DBUS_TYPE_BOOLEAN){dbus_bool_t b; dbus_message_iter_get_basic(&vv,&b); conn=b;}}
                    }
                    dbus_message_iter_next(&props);
                }
                if (conn) ++connected;
            }
            dbus_message_iter_next(&ifaceDict);
        }
        dbus_message_iter_next(&dict);
    }
    dbus_message_unref(reply);
    if (!powered) return "BT off";
    if (connected>0) return "BT " + std::to_string(connected) + " dev";
    return "BT on";
#else
    return "";
#endif
}

std::string provider_bluetooth(const Opts& o, bool simulate) {
    (void)o;
    if (simulate) return "BT on";
    std::string dbus = provider_bluetooth_dbus_try();
    if (!dbus.empty()) return dbus;
    std::string out = run_capture("bluetoothctl show 2>/dev/null | grep Powered");
    if (out.find("yes")!=std::string::npos) {
        std::string devs = run_capture("bluetoothctl devices Connected 2>/dev/null | wc -l");
        int n = std::atoi(devs.c_str());
        if (n>0) return "BT " + std::to_string(n) + " dev";
        return "BT on";
    }
    if (out.find("no")!=std::string::npos) return "BT off";
    return "BT --";
}

std::string provider_app_dock(const Opts& o, bool simulate) {
    std::string pinned_s = o.get("pinned", "firefox,kitty,code");
    auto pinned = split_csv(pinned_s);
    if (pinned.empty()) pinned = {"firefox","kitty","code"};
    if (simulate) {
        std::string out;
        for (size_t i=0;i<pinned.size();++i) {
            if (i) out += "  ";
            out += (i%2==0 ? "\u25cf " : "\u25cb ") + pinned[i];
        }
        return out;
    }
    std::string json = run_capture("hyprctl -j clients 2>/dev/null");
    std::map<std::string,bool> running;
    for (auto& p : pinned) running[to_lower_str(p)] = false;
    if (!json.empty()) {
        jmin::Value root;
        if (jmin::parse(json, root) && root.t==jmin::Value::T::Arr) {
            for (auto& c : root.arr) {
                if (c.t!=jmin::Value::T::Obj) continue;
                const jmin::Value* cls = c.get("class");
                if (!cls) cls = c.get("initialClass");
                if (!cls || cls->t!=jmin::Value::T::Str) continue;
                std::string lc = to_lower_str(cls->s);
                for (auto& p : pinned) {
                    std::string lp = to_lower_str(p);
                    if (lc.find(lp)!=std::string::npos || lp.find(lc)!=std::string::npos) running[lp]=true;
                }
            }
        }
    }
    std::string out;
    for (size_t i=0;i<pinned.size();++i) {
        if (i) out += "  ";
        std::string lp = to_lower_str(pinned[i]);
        bool is = running.count(lp) ? running[lp] : false;
        out += (is ? "\u25cf " : "\u25cb ") + pinned[i];
    }
    return out.empty() ? "--" : out;
}

std::string provider_systray(const Opts& o, bool simulate) {
    (void)o;
    if (simulate) return "TRAY 3";
    // D-Bus StatusNotifierWatcher: count registered items
#ifdef HAVE_DBUS
    DBusError err; dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (conn && !dbus_error_is_set(&err)) {
        DBusMessage* msg = dbus_message_new_method_call("org.kde.StatusNotifierWatcher",
            "/StatusNotifierWatcher","org.kde.StatusNotifierWatcher","RegisteredStatusNotifierItems");
        // Actually it's a property; try Properties Get
        DBusMessage* prop = dbus_help::prop_get(conn, "org.kde.StatusNotifierWatcher",
            "/StatusNotifierWatcher","org.kde.StatusNotifierWatcher","RegisteredStatusNotifierItems");
        if (prop) {
            DBusMessageIter it, var, arr;
            dbus_message_iter_init(prop,&it);
            if (dbus_message_iter_get_arg_type(&it)==DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&it,&var);
                if (dbus_message_iter_get_arg_type(&var)==DBUS_TYPE_ARRAY) {
                    dbus_message_iter_recurse(&var,&arr);
                    int n=0; while(dbus_message_iter_get_arg_type(&arr)==DBUS_TYPE_STRING){++n; dbus_message_iter_next(&arr);}
                    dbus_message_unref(prop);
                    if (n>=0) return "TRAY " + std::to_string(n);
                }
            }
            dbus_message_unref(prop);
        }
        if (msg) dbus_message_unref(msg);
    }
    if (dbus_error_is_set(&err)) dbus_error_free(&err);
#endif
    return "TRAY --";
}

std::string provider_workspace(const Opts& o, bool simulate) {
    if (simulate) return "1 2\u00b73";
    const std::string json = run_capture("hyprctl -j workspaces 2>/dev/null");
    jmin::Value root;
    if (json.empty() || !jmin::parse(json, root) ||
        root.t != jmin::Value::T::Arr)
        return "--";

    // The active workspace id comes from `activeworkspace` (this Hyprland
    // build does not set a "focused" flag in the workspaces list).
    int active_id = -1;
    std::string active_mon;
    const std::string act_json =
        run_capture("hyprctl -j activeworkspace 2>/dev/null");
    jmin::Value act;
    if (!act_json.empty() && jmin::parse(act_json, act) &&
        act.t == jmin::Value::T::Obj) {
        if (const auto* idv = act.get("id");
            idv && idv->t == jmin::Value::T::Int)
            active_id = static_cast<int>(idv->i);
        if (const auto* monv = act.get("monitor");
            monv && monv->t == jmin::Value::T::Str)
            active_mon = monv->s;
    }

    // Group workspace ids by monitor.
    std::map<std::string, std::vector<std::string>> monitors;
    for (const auto& w : root.arr) {
        if (w.t != jmin::Value::T::Obj) continue;
        const jmin::Value* idv = w.get("id");
        const jmin::Value* monv = w.get("monitor");
        if (!idv || idv->t != jmin::Value::T::Int) continue;
        const std::string mon = monv && monv->t == jmin::Value::T::Str
                                    ? monv->s
                                    : "all";
        monitors[mon].push_back(std::to_string(idv->i));
    }

    std::string out;
    for (const auto& [mon, ids] : monitors) {
        std::string part;
        for (const auto& id : ids) {
            if (!part.empty()) part += ' ';
            if (active_id >= 0 && id == std::to_string(active_id))
                part += "\u00b7";
            part += id;
        }
        if (!out.empty()) out += "   ";
        out += part;
    }
    return out.empty() ? "--" : out;
}

const char* provider_name(const std::string& name) {
    if (name == "clock") return "clock";
    if (name == "sysinfo") return "sysinfo";
    if (name == "audio") return "audio";
    if (name == "brightness") return "brightness";
    if (name == "workspace") return "workspace";
    if (name == "wifi") return "wifi";
    if (name == "bluetooth") return "bluetooth";
    if (name == "app-dock") return "app-dock";
    if (name == "systray") return "systray";
    return nullptr;
}

int default_interval(const std::string& name) {
    if (name == "clock") return 1000;
    if (name == "sysinfo") return 2000;
    if (name == "audio") return 500;
    if (name == "brightness") return 1000;
    if (name == "workspace") return 1000;
    if (name == "wifi") return 3000;
    if (name == "bluetooth") return 3000;
    if (name == "app-dock") return 1000;
    if (name == "systray") return 2000;
    return 1000;
}

std::string collect(const std::string& name, const Opts& o, bool simulate) {
    if (name == "clock") return provider_clock(o, simulate);
    if (name == "sysinfo") return provider_sysinfo(o, simulate);
    if (name == "audio") return provider_audio(o, simulate);
    if (name == "brightness") return provider_brightness(o, simulate);
    if (name == "workspace") return provider_workspace(o, simulate);
    if (name == "wifi") return provider_wifi(o, simulate);
    if (name == "bluetooth") return provider_bluetooth(o, simulate);
    if (name == "app-dock") return provider_app_dock(o, simulate);
    if (name == "systray") return provider_systray(o, simulate);
    return "--";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: otakud-mod <module> [opts] [--simulate] [--once] "
                     "[--daemonize]\n");
        return 1;
    }

    std::string name = argv[1];
    if (!provider_name(name)) {
        std::fprintf(stderr, "otakud-mod: unknown module '%s'\n", name.c_str());
        return 1;
    }

    Opts opts;
    bool simulate = false;
    bool once = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--simulate") simulate = true;
        else if (a == "--once") once = true;
        else opts = parse_opts(a);
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::string region = "mod-" + [&] {
        std::string u;
        for (char c : name) {
            if (std::isalnum(static_cast<unsigned char>(c)))
                u += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            else
                u += '-';
        }
        return u;
    }();

    void* region_ptr = shm_open_region(region.c_str(), kRingRegionSize, /*create=*/true);
    if (!region_ptr) {
        std::fprintf(stderr, "otakud-mod: cannot create shm region '%s'\n",
                     region.c_str());
        return 1;
    }

    RingWriter writer;
    writer.init(region_ptr, kRingRegionSize, getpid());
    if (!writer.ready()) {
        shm_close_region(region_ptr, kRingRegionSize, region.c_str(), true);
        return 1;
    }

    // Published samples. Unpublish + unlink the region on exit.
    struct Cleanup {
        RingWriter* w;
        void* base;
        const char* reg;
        ~Cleanup() {
            w->shutdown();
            shm_close_region(base, kRingRegionSize, reg, /*unlink=*/true);
        }
    } cleanup{&writer, region_ptr, region.c_str()};

    auto now_ns = [] {  // monotonic clock in nanoseconds
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
               static_cast<uint64_t>(ts.tv_nsec);
    };

    uint64_t seq = 0;
    const int interval_ms = opts.get_int("interval_ms", default_interval(name));

    std::string text = collect(name, opts, simulate);
    ModuleSample sample;
    sample.seq = ++seq;
    std::strncpy(sample.text, text.c_str(), sizeof(sample.text) - 1);

    if (once) {
        // Two samples so delta-based modules (sysinfo) return a real value.
        usleep(150'000);
        text = collect(name, opts, simulate);
        sample.seq = ++seq;
        std::strncpy(sample.text, text.c_str(), sizeof(sample.text) - 1);
        writer.set_heartbeat(now_ns());
        writer.push(&sample, sizeof(sample));
        std::printf("%s: %s\n", name.c_str(), sample.text);
        return 0;
    }

    while (g_running) {
        writer.set_heartbeat(now_ns());
        writer.push(&sample, sizeof(sample));

        int remaining = interval_ms;
        while (remaining > 0 && g_running) {
            const int step = remaining > 50 ? 50 : remaining;
            usleep(static_cast<useconds_t>(step) * 1000);
            remaining -= step;
        }
        text = collect(name, opts, simulate);
        std::strncpy(sample.text, text.c_str(), sizeof(sample.text) - 1);
        sample.seq = ++seq;
    }

    return 0;
}