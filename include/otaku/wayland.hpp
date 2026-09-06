#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wayland-client.h>

#include "otaku/config.hpp"

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

    // Visibility used by auto-hide edges: hidden surfaces shrink to a thin
    // trigger strip instead of being unmapped.
    virtual bool visible() const = 0;
    virtual void set_visible(bool visible) = 0;

    // Called by the surface implementation after a size/visibility change so
    // the owner (the Frame) can redraw the new buffer. May be null.
    std::function<void()> on_resize;
};

// The Wayland client context shared by all surfaces.
struct Display {
    bool alive{true};  // false once the connection is closed/crashed
    wl_display* display{nullptr};
    wl_registry* registry{nullptr};
    wl_compositor* compositor{nullptr};
    wl_shm* shm{nullptr};
    zwlr_layer_shell_v1* layer_shell{nullptr};
    xdg_wm_base* xdg_base{nullptr};
    zxdg_output_manager_v1* xdg_output_manager{nullptr};
    ext_session_lock_manager_v1* session_lock{nullptr};
    wl_seat* seat{nullptr};      // bound if the compositor has one
    wl_pointer* pointer{nullptr};  // created when the seat has pointer capability
};

// Connect to the Wayland display, bind globals, collect outputs (if `out`
// is non-null it is filled with one Output per wl_output advertised).
// `need_layer_shell` requires wlr-layer-shell (true for the daemon, whose
// frames are layer surfaces; false for the standalone toplevel `preview`,
// which only needs xdg-shell). Returns false on failure; `err` holds a message.
bool display_connect(Display& d, std::vector<Output>* out, std::string& err,
                     bool need_layer_shell = true);

// Poll and dispatch pending events (non-blocking). Returns false if display closed.
bool display_poll(Display& d);

// Wait up to `timeout_ms` for events, dispatching any that arrive.
// Returns false if the display was closed. Used to sleep without burning CPU.
bool display_wait(Display& d, int timeout_ms);

void display_disconnect(Display& d);

// Create a layer-shell surface (production bar). `output` nullptr = all.
// `auto_hide` shrinks the surface to a thin trigger strip and expands it
// (plus exclusive zone) while the pointer hovers it (edge auto-hide).
std::unique_ptr<ISurface> create_layer_surface(Display& d,
                                               Output* output,
                                               std::string ns,
                                               Anchor anchor,
                                               int32_t bar_size,
                                               bool exclusive_zone,
                                               bool auto_hide = false);

// Create a normal toplevel window (used by `otakushell preview`).
// `width`/`height` are the window's default size in pixels. `close_flag`
// (optional) is set to false when the user closes the window, so the caller's
// event loop can quit.
std::unique_ptr<ISurface> create_toplevel_surface(Display& d, std::string title,
                                                  int32_t width, int32_t height,
                                                  std::atomic<bool>* close_flag);

}  // namespace otaku
