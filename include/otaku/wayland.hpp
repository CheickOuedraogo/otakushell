#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <wayland-client.h>

#include "wlr-layer-shell-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-output-client-protocol.h"
#include "ext-session-lock-client-protocol.h"

namespace otaku {

// --- Output / monitor -----------------------------------------------------
struct Output {
    uint32_t name{0};
    std::string make;
    std::string model;
    int32_t width{0};
    int32_t height{0};
    int32_t scale{1};
    bool usable{false};
};

// --- Abstraction over a Wayland surface -----------------------------------
// The daemon uses LayerSurface (anchored panels); the CLI `preview` uses
// ToplevelSurface (a normal window). Both expose the same drawing surface.
class ISurface {
public:
    virtual ~ISurface() = default;

    // Configure the surface size in pixels.
    virtual void set_size(int32_t width, int32_t height) = 0;
    virtual void present() = 0;  // attach buffer + commit

    virtual int32_t width() const = 0;
    virtual int32_t height() const = 0;
    virtual wl_buffer* buffer() const = 0;
    virtual void* pixel_data() = 0;
    virtual size_t buffer_size() const = 0;
    virtual int32_t stride() const = 0;
};

// The Wayland client context shared by all surfaces.
struct Display {
    wl_display* display{nullptr};
    wl_registry* registry{nullptr};
    wl_compositor* compositor{nullptr};
    wl_shm* shm{nullptr};
    zwlr_layer_shell_v1* layer_shell{nullptr};
    xdg_wm_base* xdg_base{nullptr};
    zxdg_output_manager_v1* xdg_output_manager{nullptr};
    ext_session_lock_manager_v1* session_lock{nullptr};
};

// Connect to the Wayland display, bind globals, collect outputs (if `out`
// is non-null it is filled with one Output per wl_output advertised).
// Returns false on failure; `err` holds a message.
bool display_connect(Display& d, std::vector<Output>* out, std::string& err);

// Poll and dispatch pending events (non-blocking). Returns false if display closed.
bool display_poll(Display& d);

void display_disconnect(Display& d);

// Create a layer-shell surface (production bar). `output` nullptr = all.
std::unique_ptr<ISurface> create_layer_surface(Display& d,
                                               Output* output,
                                               std::string ns,
                                               uint32_t anchor,
                                               int32_t height,
                                               bool exclusive_zone);

// Create a normal toplevel window (used by `otakushell preview`).
std::unique_ptr<ISurface> create_toplevel_surface(Display& d, std::string title);

}  // namespace otaku
