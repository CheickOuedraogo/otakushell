#pragma once

#include <memory>
#include <string>
#include <vector>

#include "otaku/config.hpp"

namespace otaku {

class ISurface;
class IModule;
class ModuleSupervisor;
struct Display;
struct ShellConfig;
struct Output;

// A Frame is a Wayland surface that lays out modules in positions.
// It owns its surface and the modules assigned to it by the config order.
class Frame {
public:
    explicit Frame(const FrameSpec& spec);
    ~Frame();

    const std::string& id() const { return spec_.id; }
    const FrameSpec& spec() const { return spec_; }

    void set_theme(const ThemeColors& colors);

    // Instantiate the modules declared in spec.order, backed by `sup`'s
    // producers. Uses the config for per-module options and the font.
    void attach_modules(ModuleSupervisor& sup, const ShellConfig& cfg);

    // Attach the backing surface to this frame (daemon: layer; preview: toplevel).
    void set_surface(std::unique_ptr<ISurface> surface);

    // Resize handling: set the surface size and re-layout modules.
    void configure(int32_t width, int32_t height);

    void render();

    // True once the backing surface holds a sized, allocated buffer (i.e. it
    // has been configured and can be presented). Used by the lockscreen to
    // verify every lock surface is really paintable before trusting the lock.
    bool ready() const;

private:
    FrameSpec spec_;
    ThemeColors theme_;
    std::string font_{"Sans 11"};
    std::unique_ptr<ISurface> surface_;
    std::vector<std::shared_ptr<IModule>> modules_;
};

// Build one Frame per configured frame (fullscreen lockscreen is skipped),
// each backed by a layer surface. Used by the daemon and re-run on hot-reload.
// Auto-hide edges (FrameSpec::hidden) start collapsed to their trigger strip.
std::vector<std::unique_ptr<Frame>> build_frames(Display& d,
                                                 ModuleSupervisor& sup,
                                                 const ShellConfig& cfg,
                                                 const std::vector<Output>& outputs);

}  // namespace otaku