#include "otaku/wayland.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

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

    void destroy() {
        for (int i = 0; i < 2; ++i) {
            if (buffers[i]) wl_buffer_destroy(buffers[i]);
            if (data[i]) munmap(data[i], size);
        }
        buffers[0] = buffers[1] = nullptr;
        data[0] = data[1] = nullptr;
        w = h = 0;
    }

    void* front_data() { return data[front]; }
};

// ---------------------------------------------------------------------------
// Layer-surface implementation (production bars, anchored panels)
// ---------------------------------------------------------------------------
class LayerSurface final : public ISurface {
public:
    LayerSurface(Display& d, Output* output, std::string ns, Anchor anchor,
                 int32_t bar_size, bool exclusive)
        : display_(d), namespace_(std::move(ns)), anchor_(anchor) {
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
            zwlr_layer_surface_v1_set_size(layer_, 0, static_cast<uint32_t>(bar_size));
        } else {
            zwlr_layer_surface_v1_set_size(layer_, static_cast<uint32_t>(bar_size), 0);
        }
        zwlr_layer_surface_v1_set_anchor(layer_, flags);
        if (exclusive)
            zwlr_layer_surface_v1_set_exclusive_zone(layer_, bar_size);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            layer_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

        zwlr_layer_surface_v1_add_listener(layer_, &layer_listener_, this);
        wl_surface_commit(wl_surface_);
    }

    ~LayerSurface() override {
        if (display_.alive && layer_) zwlr_layer_surface_v1_destroy(layer_);
        shm_.destroy();
    }

    void set_size(int32_t width, int32_t height) override {
        if (width == shm_.w && height == shm_.h) {
            present();  // re-present after resize-less configure
            return;
        }
        if (shm_.data[0]) shm_.destroy();
        if (shm_.create(width, height)) {
            anchor_width_ = width;
            anchor_height_ = height;
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

private:
    static void handle_configure(void* data, zwlr_layer_surface_v1*, uint32_t serial,
                                 uint32_t w, uint32_t h) {
        auto* self = static_cast<LayerSurface*>(data);
        zwlr_layer_surface_v1_ack_configure(self->layer_, serial);
        self->set_size(static_cast<int32_t>(w), static_cast<int32_t>(h));
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
    int32_t anchor_width_{0}, anchor_height_{0};
};

}  // namespace

bool display_connect(Display& d, std::vector<Output>* outputs, std::string& err) {
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
    else if (!d.layer_shell)
        err = "compositor does not expose wlr-layer-shell (not Hyprland/wlroots?)";
    else if (!d.xdg_base)
        err = "compositor does not expose xdg-shell";
    if (!err.empty()) return false;
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
                                               int32_t bar_size, bool exclusive) {
    return std::make_unique<LayerSurface>(d, output, std::move(ns), anchor,
                                          bar_size, exclusive);
}

std::unique_ptr<ISurface> create_toplevel_surface(Display&, std::string) {
    return nullptr;  // implemented in step 2
}

}  // namespace otaku
