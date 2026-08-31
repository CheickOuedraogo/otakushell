#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace otaku {

class ISurface;

// A Module is a self-contained UI component rendered by the shell.
// Modules are written once and used by both the daemon (production data)
// and the CLI `preview` (simulated data), selected via their data source.
class IModule {
public:
    virtual ~IModule() = default;

    virtual const char* name() const = 0;

    // Update state (called on poll/tick). `now_ms` is monotonic time.
    virtual void tick(uint64_t now_ms) = 0;

    // Render this module at pixel (0,0) in `surface`. Returns the width used
    // (so the layout can place the next module to the right).
    virtual int render(ISurface& s) = 0;

    // Optional width hint used by the layout before first render.
    virtual int preferred_width() const = 0;
};

// Factory: create a module by name. Returns nullptr if unknown.
std::shared_ptr<IModule> create_module(const std::string& name, bool simulate);

}  // namespace otaku
