#include "otaku/wayland.hpp"

#include <stdexcept>
#include <string>

#include "wlr-layer-shell-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-output-client-protocol.h"
#include "ext-session-lock-client-protocol.h"

namespace otaku {

bool display_connect(Display& d, std::string& err) {
    d.display = wl_display_connect(nullptr);
    if (!d.display) {
        err = "cannot connect to a Wayland compositor (is WAYLAND_DISPLAY set?)";
        return false;
    }
    return true;
}

bool display_poll(Display& d) {
    if (!d.display) return false;
    int ret = wl_display_dispatch_pending(d.display);
    if (ret < 0) return false;
    return true;
}

void display_disconnect(Display& d) {
    if (d.display) wl_display_disconnect(d.display);
    d.display = nullptr;
}

std::unique_ptr<ISurface> create_layer_surface(Display&, Output*, std::string,
                                               uint32_t, int32_t, bool) {
    // Implemented in step 2 (surface abstraction).
    return nullptr;
}

std::unique_ptr<ISurface> create_toplevel_surface(Display&, std::string) {
    // Implemented in step 2 (surface abstraction).
    return nullptr;
}

}  // namespace otaku
