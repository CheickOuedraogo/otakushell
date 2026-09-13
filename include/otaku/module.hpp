#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "otaku/supervisor.hpp"

namespace otaku {

class ISurface;

// Layout context handed to IModule::render().
struct RenderContext {
    std::string font;   // pango font description, e.g. "Sans 11"
    uint32_t fg{0};     // text color (ARGB)
    int x{0};           // left pixel where this module draws
    int height{0};      // frame height (bar), for vertical centering
};

// A Module is a self-contained UI component rendered by the shell. Data comes
// from a producer process (`otakud-mod`) through a shared ring buffer; the
// module only renders the latest sample. Modules are written once and used by
// both the daemon (real data) and the CLI `preview` (simulated data).
class IModule {
public:
    virtual ~IModule() = default;

    virtual const char* name() const = 0;

    // Update state (called on poll/tick). `now_ms` is monotonic time.
    virtual void tick(uint64_t now_ms) = 0;

    // Render this module at pixel ctx.x in `surface`. Returns the width used
    // (so the layout can place the next module to the right).
    virtual int render(ISurface& s, const RenderContext& ctx) = 0;

    // Optional width hint used by the layout before first render.
    virtual int preferred_width() const = 0;

    // Same as render() but purely measuring: the pixel width this module
    // would occupy (including trailing spacing), without drawing anything.
    // Used by the layout to center a row (lockscreen).
    virtual int measure(const RenderContext& ctx) const = 0;
};

// Factory: create a module by name, backed by the supervisor's producer.
// `opts` is the serialized `[modules.<name>]` option string passed to the
// producer. Returns nullptr for unknown modules (silently skipped).
std::shared_ptr<IModule> create_module(const std::string& name,
                                       ModuleSupervisor& sup,
                                       const std::string& opts);

// True when a module producer is implemented for `name` (mirrors the
// supported set in otakud-mod). Unknown modules in a frame's order are
// skipped without spawning any process.
bool module_supported(const std::string& name);

}  // namespace otaku