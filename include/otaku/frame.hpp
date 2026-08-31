#pragma once

#include <memory>
#include <string>
#include <vector>

#include "otaku/config.hpp"

namespace otaku {

class ISurface;
class IModule;

// A Frame is a Wayland surface that lays out modules in positions.
// It owns its surface and the modules assigned to it by the config order.
class Frame {
public:
    explicit Frame(const FrameSpec& spec);
    ~Frame();

    const std::string& id() const { return spec_.id; }
    const FrameSpec& spec() const { return spec_; }

    // Attach the backing surface to this frame (daemon: layer; preview: toplevel).
    void set_surface(std::unique_ptr<ISurface> surface);

    // Resize handling: set the surface size and re-layout modules.
    void configure(int32_t width, int32_t height);

    void render();

private:
    FrameSpec spec_;
    std::unique_ptr<ISurface> surface_;
    std::vector<std::shared_ptr<IModule>> modules_;
};

}  // namespace otaku
