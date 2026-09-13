#include "otaku/wayland.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ext-session-lock-client-protocol.h"
#include "wlr-layer-shell-client-protocol.h"
#include "xdg-output-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace otaku {

namespace {

// ---------------------------------------------------------------------------
// Anonymous shared-memory file for a wl_shm pool
// ---------------------------------------------------------------------------
int create_anonymous_file(size_t size) {
    static const char tmpl[] = "/otaku-shm-XXXXXX";
    char path[64];
    std::snprintf(path, sizeof(path), "%s%s", "/tmp", tmpl);
    int fd = mkstemp(path);
    if (fd < 0) return fd;
    unlink(path);
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------------------
// Registry / global binding
// ---------------------------------------------------------------------------
struct RegistryCtx {
    Display* d;
    std::vector<Output>* outputs;
};

void registry_global(void* data, wl_registry*, uint32_t name, const char* iface,
                     uint32_t version) {
    auto* ctx = static_cast<RegistryCtx*>(data);
    Display* d = ctx->d;

    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        d->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(d->registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
        d->shm = static_cast<wl_shm*>(
            wl_registry_bind(d->registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        d->layer_shell = static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(
            d->registry, name, &zwlr_layer_shell_v1_interface, 4));
    } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        d->xdg_base = static_cast<xdg_wm_base*>(wl_registry_bind(
            d->registry, name, &xdg_wm_base_interface, 1));
    } else if (std::strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
        d->xdg_output_manager = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(d->registry, name, &zxdg_output_manager_v1_interface, 3));
    } else if (std::strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0) {
        d->session_lock = static_cast<ext_session_lock_manager_v1*>(wl_registry_bind(
            d->registry, name, &ext_session_lock_manager_v1_interface, 1));
    } else if (std::strcmp(iface, wl_output_interface.name) == 0) {
        ctx->outputs->push_back(Output{name, {}, {}, 0, 0, 1, false});
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
        d->seat = static_cast<wl_seat*>(
            wl_registry_bind(d->registry, name, &wl_seat_interface, 1));
    }
}

void registry_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener registry_listener = {registry_global, registry_remove};

// ---------------------------------------------------------------------------
// Double-buffered wl_shm surface
// ---------------------------------------------------------------------------
struct ShmSurface {
    wl_compositor* compositor{nullptr};
    wl_shm* shm{nullptr};
    wl_buffer* buffers[2]{nullptr, nullptr};
    void* data[2]{nullptr, nullptr};
    int32_t w{0}, h{0};
    size_t stride{0};
    size_t size{0};
    int front{0};

    bool create(int32_t width, int32_t height) {
        if (width <= 0 || height <= 0) return false;
        w = width;
        h = height;
        stride = static_cast<size_t>(width) * 4;  // ARGB8888
        size = stride * static_cast<size_t>(h);

        for (int i = 0; i < 2; ++i) {
            int fd = create_anonymous_file(size);
            if (fd < 0) return false;
            void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (map == MAP_FAILED) {
                close(fd);
                return false;
            }
            wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(size));
            buffers[i] = wl_shm_pool_create_buffer(pool, 0, width, height,
                                                   static_cast<int32_t>(stride),
                                                   WL_SHM_FORMAT_ARGB8888);
            wl_shm_pool_destroy(pool);
            close(fd);
            data[i] = map;
        }
        return true;
    }

    // destroy(wayland_ok): when the Wayland connection is already closed
    // (e.g. a compositor protocol error), only unmap memory and never touch
    // the Wayland proxies (that would marshal over a dead connection).
    void destroy(bool wayland_ok = true) {
        for (int i = 0; i < 2; ++i) {
            if (wayland_ok && buffers[i]) wl_buffer_destroy(buffers[i]);
            if (data[i]) munmap(data[i], size);
        }
        buffers[0] = buffers[1] = nullptr;
        data[0] = data[1] = nullptr;
        w = h = 0;
    }

    void* front_data() { return data[front]; }
};

// ---------------------------------------------------------------------------
// Auto-hide edge registry
// ---------------------------------------------------------------------------
// Layer surfaces created with `auto_hide` register their wl_surface here.
// Pointer enter/leave on that surface expands/shrinks the edge.
struct EdgeEntry {
    wl_surface* wl_surf{nullptr};
    ISurface* surf{nullptr};
};
std::vector<EdgeEntry> g_edge_registry;

// ---------------------------------------------------------------------------
// Layer-surface implementation (production bars, anchored panels)
// ---------------------------------------------------------------------------
class LayerSurface final : public ISurface {
public:
    LayerSurface(Display& d, Output* output, std::string ns, Anchor anchor,
                 int32_t bar_size, bool exclusive, bool auto_hide)
        : display_(d),
          namespace_(std::move(ns)),
          anchor_(anchor),
          bar_size_(bar_size),
          exclusive_(exclusive),
          auto_hide_(auto_hide) {
        shm_.compositor = d.compositor;
        shm_.shm = d.shm;
        wl_surface_ = wl_compositor_create_surface(d.compositor);

        wl_output* out = output
                             ? static_cast<wl_output*>(wl_registry_bind(
                                   d.registry, output->name, &wl_output_interface, 1))
                             : nullptr;
        layer_ = zwlr_layer_shell_v1_get_layer_surface(
            d.layer_shell, wl_surface_, out,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP, namespace_.c_str());

        const uint32_t TOP = 1, BOTTOM = 2, LEFT = 4, RIGHT = 8;
        const bool horizontal = (anchor == Anchor::Top || anchor == Anchor::Bottom);
        // A bar with size 0 on its axis is stretched by the compositor, which
        // requires the complementary anchors along that axis to be present.
        uint32_t flags = 0;
        switch (anchor) {
            case Anchor::Top:    flags = TOP | LEFT | RIGHT; break;
            case Anchor::Bottom: flags = BOTTOM | LEFT | RIGHT; break;
            case Anchor::Left:   flags = LEFT | TOP | BOTTOM; break;
            case Anchor::Right:  flags = RIGHT | TOP | BOTTOM; break;
            case Anchor::Full:
            case Anchor::None:
                flags = (LEFT | RIGHT | TOP | BOTTOM);
                break;
        }

        if (horizontal) {
            shown_w_ = 0;
            shown_h_ = bar_size;
            hidden_w_ = 0;
            hidden_h_ = 1;  // 1px-tall trigger strip along the whole width
        } else {
            shown_w_ = bar_size;
            shown_h_ = 0;
            hidden_w_ = 1;  // 1px-wide trigger strip along the whole height
            hidden_h_ = 0;
        }

        zwlr_layer_surface_v1_set_size(layer_, static_cast<uint32_t>(shown_w_),
                                       static_cast<uint32_t>(shown_h_));
        zwlr_layer_surface_v1_set_anchor(layer_, flags);
        if (exclusive)
            zwlr_layer_surface_v1_set_exclusive_zone(layer_, bar_size);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            layer_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

        zwlr_layer_surface_v1_add_listener(layer_, &layer_listener_, this);
        wl_surface_commit(wl_surface_);

        // Register for pointer-hover auto-hide on this surface.
        if (auto_hide_) g_edge_registry.push_back({wl_surface_, this});
    }

    ~LayerSurface() override {
        if (auto_hide_) {
            for (auto it = g_edge_registry.begin(); it != g_edge_registry.end(); ++it) {
                if (it->surf == this) {
                    g_edge_registry.erase(it);
                    break;
                }
            }
        }
        if (display_.alive) {
            if (layer_) zwlr_layer_surface_v1_destroy(layer_);
            if (wl_surface_) wl_surface_destroy(wl_surface_);
        }
        shm_.destroy(display_.alive);
    }

    wl_surface* surface_handle() const { return wl_surface_; }

    void set_size(int32_t width, int32_t height) override {
        if (width == shm_.w && height == shm_.h) {
            present();  // re-present after resize-less configure
            return;
        }
        if (shm_.data[0]) shm_.destroy();
        shm_.create(width, height);  // buffer sized; owner redraws via on_resize
    }

    void present() override {
        if (!wl_surface_ || !shm_.data[0]) return;
        wl_surface_attach(wl_surface_, shm_.buffers[shm_.front], 0, 0);
        wl_surface_damage_buffer(wl_surface_, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(wl_surface_);
        shm_.front = (shm_.front + 1) % 2;
    }

    int32_t width() const override { return shm_.w; }
    int32_t height() const override { return shm_.h; }
    wl_buffer* buffer() const override { return shm_.buffers[shm_.front]; }
    void* pixel_data() override { return shm_.data[shm_.front]; }
    size_t buffer_size() const override { return shm_.size; }
    int32_t stride() const override { return static_cast<int32_t>(shm_.stride); }

    bool visible() const override { return auto_hide_ ? visible_ : true; }

    wl_surface* native_surface() const override { return wl_surface_; }

    // Auto-hide: show the full edge (with exclusive zone) or collapse to the
    // 1px trigger strip (no exclusive zone). A no-op unless auto_hide_.
    void set_visible(bool v) override {
        if (!auto_hide_ || v == visible_) return;
        visible_ = v;
        if (v) {
            zwlr_layer_surface_v1_set_size(layer_, static_cast<uint32_t>(shown_w_),
                                           static_cast<uint32_t>(shown_h_));
            if (exclusive_)
                zwlr_layer_surface_v1_set_exclusive_zone(layer_, bar_size_);
        } else {
            zwlr_layer_surface_v1_set_size(layer_, static_cast<uint32_t>(hidden_w_),
                                           static_cast<uint32_t>(hidden_h_));
            zwlr_layer_surface_v1_set_exclusive_zone(layer_, 0);
        }
        wl_surface_commit(wl_surface_);
    }

private:
    static void handle_configure(void* data, zwlr_layer_surface_v1*, uint32_t serial,
                                 uint32_t w, uint32_t h) {
        auto* self = static_cast<LayerSurface*>(data);
        zwlr_layer_surface_v1_ack_configure(self->layer_, serial);
        self->set_size(static_cast<int32_t>(w), static_cast<int32_t>(h));
        // The front buffer changed size; let the owner re-render it.
        if (self->on_resize) self->on_resize();
    }
    static void handle_close(void*, zwlr_layer_surface_v1*) {}

    static constexpr zwlr_layer_surface_v1_listener layer_listener_ = {
        handle_configure, handle_close};

    Display& display_;
    std::string namespace_;
    Anchor anchor_{Anchor::None};
    zwlr_layer_surface_v1* layer_{nullptr};
    wl_surface* wl_surface_{nullptr};
    ShmSurface shm_{};
    int32_t bar_size_{0};
    bool exclusive_{false};
    bool auto_hide_{false};
    bool visible_{true};
    int32_t shown_w_{0}, shown_h_{0};
    int32_t hidden_w_{0}, hidden_h_{0};
};

// ---------------------------------------------------------------------------
// Toplevel-surface implementation (a normal xdg window, used by preview)
// ---------------------------------------------------------------------------
class ToplevelSurface final : public ISurface {
public:
    ToplevelSurface(Display& d, std::string title, int32_t width, int32_t height,
                    std::atomic<bool>* close_flag)
        : display_(d), title_(std::move(title)), close_flag_(close_flag) {
        shm_.compositor = d.compositor;
        shm_.shm = d.shm;
        wl_surface_ = wl_compositor_create_surface(d.compositor);

        xdg_surface_ =
            xdg_wm_base_get_xdg_surface(d.xdg_base, wl_surface_);
        xdg_toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
        xdg_toplevel_set_title(xdg_toplevel_, title_.c_str());
        xdg_toplevel_set_app_id(xdg_toplevel_, "otakushell-preview");

        // Pin the window to its bar/edge dimensions (fixed size, no resize).
        if (width > 0 && height > 0) {
            xdg_toplevel_set_min_size(xdg_toplevel_, width, height);
            xdg_toplevel_set_max_size(xdg_toplevel_, width, height);
        }

        xdg_surface_add_listener(xdg_surface_, &surface_listener_, this);
        xdg_toplevel_add_listener(xdg_toplevel_, &toplevel_listener_, this);

        // Fallback size until the compositor sends a configure with a
        // non-zero dimension (then the real size wins).
        if (width > 0 && height > 0) set_size(width, height);

        wl_surface_commit(wl_surface_);
    }

    ~ToplevelSurface() override {
        if (display_.alive) {
            if (xdg_toplevel_) xdg_toplevel_destroy(xdg_toplevel_);
            if (xdg_surface_) xdg_surface_destroy(xdg_surface_);
            if (wl_surface_) wl_surface_destroy(wl_surface_);
        }
        shm_.destroy(display_.alive);
    }

    void set_size(int32_t width, int32_t height) override {
        if (width == shm_.w && height == shm_.h) {
            present();  // re-present after a resize-less configure
            return;
        }
        if (shm_.data[0]) shm_.destroy();
        if (shm_.create(width, height)) {
            width_ = width;
            height_ = height;
        }
    }

    void present() override {
        if (!wl_surface_ || !shm_.data[0]) return;
        wl_surface_attach(wl_surface_, shm_.buffers[shm_.front], 0, 0);
        wl_surface_damage_buffer(wl_surface_, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(wl_surface_);
        shm_.front = (shm_.front + 1) % 2;
    }

    int32_t width() const override { return shm_.w; }
    int32_t height() const override { return shm_.h; }
    wl_buffer* buffer() const override { return shm_.buffers[shm_.front]; }
    void* pixel_data() override { return shm_.data[shm_.front]; }
    size_t buffer_size() const override { return shm_.size; }
    int32_t stride() const override { return static_cast<int32_t>(shm_.stride); }

    bool visible() const override { return true; }
    void set_visible(bool) override { /* windows are always visible */ }

    wl_surface* native_surface() const override { return wl_surface_; }

private:
    static void handle_surface_configure(void* data, xdg_surface*, uint32_t serial) {
        auto* self = static_cast<ToplevelSurface*>(data);
        xdg_surface_ack_configure(self->xdg_surface_, serial);
    }

    static void handle_toplevel_configure(void* data, xdg_toplevel*, int32_t width,
                                          int32_t height, wl_array*) {
        auto* self = static_cast<ToplevelSurface*>(data);
        if (width > 0 && height > 0) {
            self->set_size(width, height);
            if (self->on_resize) self->on_resize();
        }
    }

    static void handle_toplevel_close(void* data, xdg_toplevel*) {
        auto* self = static_cast<ToplevelSurface*>(data);
        if (self->close_flag_) self->close_flag_->store(false);
    }

    static constexpr xdg_surface_listener surface_listener_ = {
        handle_surface_configure};
    static constexpr xdg_toplevel_listener toplevel_listener_ = {
        handle_toplevel_configure, handle_toplevel_close, nullptr, nullptr};

    Display& display_;
    std::string title_;
    std::atomic<bool>* close_flag_{nullptr};
    xdg_surface* xdg_surface_{nullptr};
    xdg_toplevel* xdg_toplevel_{nullptr};
    wl_surface* wl_surface_{nullptr};
    ShmSurface shm_{};
    int32_t width_{0}, height_{0};
};

// ---------------------------------------------------------------------------
// Session-lock surface implementation (one per output, exactly covers it).
// Used by the lockscreen frame (ext-session-lock). The surface must not
// commit anything before its first configure event, so the constructor only
// creates the objects; the first commit happens in render() after configure.
// ---------------------------------------------------------------------------
class LockSurface final : public ISurface {
public:
    LockSurface(Display& d, const Output& out, ext_session_lock_v1* lock)
        : display_(d) {
        shm_.compositor = d.compositor;
        shm_.shm = d.shm;
        wl_surface_ = wl_compositor_create_surface(d.compositor);
        wl_output* output = static_cast<wl_output*>(wl_registry_bind(
            d.registry, out.name, &wl_output_interface, 1));
        lock_surface_ = ext_session_lock_v1_get_lock_surface(lock, wl_surface_,
                                                             output);
ext_session_lock_surface_v1_add_listener(lock_surface_,
                                                  &lock_listener_, this);
        // intentionally no commit before the first configure
    }

    ~LockSurface() override {
        if (display_.alive) {
            if (lock_surface_) ext_session_lock_surface_v1_destroy(lock_surface_);
            if (wl_surface_) wl_surface_destroy(wl_surface_);
        }
        shm_.destroy(display_.alive);
    }

    void set_size(int32_t width, int32_t height) override {
        if (shm_.create(width, height)) present();
    }

    void present() override {
        if (!wl_surface_ || !shm_.data[0]) return;
        wl_surface_attach(wl_surface_, shm_.buffers[shm_.front], 0, 0);
        wl_surface_damage_buffer(wl_surface_, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(wl_surface_);
        shm_.front = (shm_.front + 1) % 2;
    }

    int32_t width() const override { return shm_.w; }
    int32_t height() const override { return shm_.h; }
    wl_buffer* buffer() const override { return shm_.buffers[shm_.front]; }
    void* pixel_data() override { return shm_.data[shm_.front]; }
    size_t buffer_size() const override { return shm_.size; }
    int32_t stride() const override { return static_cast<int32_t>(shm_.stride); }

    bool visible() const override { return true; }
    void set_visible(bool) override { /* lock surfaces are always visible */ }

private:
    static void handle_configure(void* data, ext_session_lock_surface_v1*, 
                                 uint32_t serial, uint32_t w, uint32_t h) {
        auto* self = static_cast<LockSurface*>(data);
        ext_session_lock_surface_v1_ack_configure(self->lock_surface_, serial);
        // The lock surface must match the acked size exactly; owner redraws
        // (first real commit happens here, after the ack).
        if (self->shm_.data[0]) self->shm_.destroy();
        if (!self->shm_.create(static_cast<int32_t>(w), static_cast<int32_t>(h))) {
            std::fprintf(stderr,
                         "lock: could not allocate %ux%u shm buffer — session "
                         "will NOT be locked\n",
                         w, h);
            return;
        }
        if (self->on_resize) self->on_resize();
    }

    static constexpr ext_session_lock_surface_v1_listener lock_listener_ = {
        handle_configure};

    Display& display_;
    ext_session_lock_surface_v1* lock_surface_{nullptr};
    wl_surface* wl_surface_{nullptr};
    ShmSurface shm_{};
};

// ---------------------------------------------------------------------------
// Seat / pointer tracking (used by auto-hide edges AND clickable windows such
// as the settings UI). Position is tracked whenever the pointer is over any
// of our surfaces; button handlers read the last tracked position.
// ---------------------------------------------------------------------------
void pointer_enter(void* data, wl_pointer*, uint32_t, wl_surface* surface,
                   wl_fixed_t sx, wl_fixed_t sy) {
    auto* d = static_cast<Display*>(data);
    d->ptr_surface = surface;
    d->ptr_x = sx;
    d->ptr_y = sy;
    for (const auto& e : g_edge_registry) {
        if (e.wl_surf == surface) {
            e.surf->set_visible(true);
            break;
        }
    }
    if (d->on_pointer_enter) d->on_pointer_enter(surface, true);
}

void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface* surface) {
    auto* d = static_cast<Display*>(data);
    d->ptr_surface = nullptr;
    for (const auto& e : g_edge_registry) {
        if (e.wl_surf == surface) {
            e.surf->set_visible(false);
            break;
        }
    }
    if (d->on_pointer_enter) d->on_pointer_enter(surface, false);
}

void pointer_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx,
                    wl_fixed_t sy) {
    auto* d = static_cast<Display*>(data);
    d->ptr_x = sx;
    d->ptr_y = sy;
    if (d->on_pointer_motion && d->ptr_surface)
        d->on_pointer_motion(d->ptr_surface, wl_fixed_to_int(sx),
                             wl_fixed_to_int(sy));
}

void pointer_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button,
                    uint32_t state) {
    auto* d = static_cast<Display*>(data);
    if (d->on_pointer_button && d->ptr_surface)
        d->on_pointer_button(d->ptr_surface, button, state,
                             wl_fixed_to_int(d->ptr_x), wl_fixed_to_int(d->ptr_y));
}

// wl_pointer version 1: only the first five events exist. The full listener
// is still filled so the build is warning-free; later events never fire at
// the version we bound.
static constexpr wl_pointer_listener g_pointer_listener = {
    pointer_enter,
    pointer_leave,
    pointer_motion,
    pointer_button,
    [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},  // axis
    nullptr,   // frame
    nullptr,   // axis_source
    nullptr,   // axis_stop
    nullptr,   // axis_discrete
    nullptr,   // axis_value120
    nullptr,   // axis_relative_direction
    nullptr,   // warp
};

// ---------------------------------------------------------------------------
// Keyboard listeners (used by the lockscreen: the compositor gives one of the
// lock surfaces keyboard focus, so key events reach on_key while locked).
// The settings window also uses them for its text fields.
// ---------------------------------------------------------------------------
void keyboard_keymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) {
    close(fd);  // we don't need the keymap; keys are matched on keycode
}

void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key,
                  uint32_t state) {
    auto* d = static_cast<Display*>(data);
    if (d->on_key) d->on_key(key, state);
}

void keyboard_modifiers(void* data, wl_keyboard*, uint32_t, uint32_t depressed,
                        uint32_t, uint32_t, uint32_t) {
    auto* d = static_cast<Display*>(data);
    d->kbd_mods = depressed;
}

// wl_keyboard version 1: the first five events exist; the rest are never
// fired at the version we bind but the listener is kept warning-free.
static constexpr wl_keyboard_listener g_keyboard_listener = {
    keyboard_keymap,
    [](void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {},  // enter
    [](void*, wl_keyboard*, uint32_t, wl_surface*) {},  // leave
    keyboard_key,
    keyboard_modifiers,
    nullptr,   // repeat_info
};

// Bind the seat's pointer and keyboard if the compositor provides them. The
// capabilities event is already queued (delivered by the roundtrip below), so
// the devices are created synchronously.
void init_seat_devices(Display& d) {
    if (!d.seat) return;
    static const wl_seat_listener seat_listener = {
        [](void* data, wl_seat* seat, uint32_t capabilities) {
            auto* dd = static_cast<Display*>(data);
            if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !dd->pointer) {
                dd->pointer = wl_seat_get_pointer(seat);
                wl_pointer_add_listener(dd->pointer, &g_pointer_listener, dd);
            } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && dd->pointer) {
                wl_pointer_destroy(dd->pointer);
                dd->pointer = nullptr;
            }
            if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !dd->keyboard) {
                dd->keyboard = wl_seat_get_keyboard(seat);
                wl_keyboard_add_listener(dd->keyboard, &g_keyboard_listener, dd);
            } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && dd->keyboard) {
                wl_keyboard_destroy(dd->keyboard);
                dd->keyboard = nullptr;
            }
        },
        nullptr  // name (seat v2+, not fired at the v1 we bound)
    };
    wl_seat_add_listener(d.seat, &seat_listener, &d);
    wl_display_roundtrip(d.display);  // deliver the capabilities event
}

}  // namespace

bool display_connect(Display& d, std::vector<Output>* outputs, std::string& err,
                     bool need_layer_shell) {
    d = Display{};
    d.display = wl_display_connect(nullptr);
    if (!d.display) {
        err = "cannot connect to a Wayland compositor (is WAYLAND_DISPLAY set?)";
        return false;
    }
    d.registry = wl_display_get_registry(d.display);
    if (!d.registry) {
        err = "failed to get registry";
        return false;
    }
    std::vector<Output> collected;
    RegistryCtx ctx{&d, outputs ? outputs : &collected};
    wl_registry_add_listener(d.registry, &registry_listener, &ctx);
    wl_display_roundtrip(d.display);
    if (!d.compositor || !d.shm)
        err = "compositor does not support wl_compositor/wl_shm";
    else if (need_layer_shell && !d.layer_shell)
        err = "compositor does not expose wlr-layer-shell (not Hyprland/wlroots?)";
    else if (!d.xdg_base)
        err = "compositor does not expose xdg-shell";
    if (!err.empty()) return false;

    init_seat_devices(d);
    return true;
}

bool display_poll(Display& d) {
    if (!d.display) return false;
    int ret = wl_display_dispatch_pending(d.display);
    if (ret < 0) {
        wl_display_flush(d.display);
        ret = wl_display_dispatch(d.display);
    }
    if (ret < 0) d.alive = false;
    return d.alive;
}

bool display_wait(Display& d, int timeout_ms) {
    if (!d.display) return false;

    struct pollfd pfd;
    pfd.fd = wl_display_get_fd(d.display);
    pfd.events = POLLIN;
    pfd.revents = 0;

    // Dispatch anything already queued first.
    if (wl_display_prepare_read(d.display) != 0) {
        wl_display_dispatch_pending(d.display);
        return true;
    }
    wl_display_flush(d.display);

    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) {
        if (errno == EINTR) {
            // A signal (e.g. SIGUSR1 reload) interrupted the poll; the caller
            // checks its flags on the next iteration.
            wl_display_cancel_read(d.display);
            return true;
        }
        d.alive = false;
        wl_display_cancel_read(d.display);
        return false;
    }
    if (pr == 0) {
        wl_display_cancel_read(d.display);
        return true;  // timeout elapsed
    }
    if (wl_display_read_events(d.display) < 0) {
        d.alive = false;
        return false;
    }
    wl_display_dispatch_pending(d.display);
    return d.alive;
}

void display_disconnect(Display& d) {
    if (d.display) wl_display_disconnect(d.display);
    d.alive = false;
    d.display = nullptr;
}

std::unique_ptr<ISurface> create_layer_surface(Display& d, Output* output,
                                               std::string ns, Anchor anchor,
                                               int32_t bar_size, bool exclusive,
                                               bool auto_hide) {
    return std::make_unique<LayerSurface>(d, output, std::move(ns), anchor,
                                          bar_size, exclusive, auto_hide);
}

std::unique_ptr<ISurface> create_toplevel_surface(Display& d, std::string title,
                                                  int32_t width, int32_t height,
                                                  std::atomic<bool>* close_flag) {
    return std::make_unique<ToplevelSurface>(d, std::move(title), width, height,
                                             close_flag);
}

std::unique_ptr<ISurface> create_lock_surface(Display& d, const Output& output,
                                              ext_session_lock_v1* lock,
                                              const std::string&) {
    return std::make_unique<LockSurface>(d, output, lock);
}

}  // namespace otaku
