#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "ext-session-lock-client-protocol.h"

#include "otaku/config.hpp"
#include "otaku/wayland.hpp"

namespace otaku {

class Display;
class Frame;
class ModuleSupervisor;
struct ShellConfig;
struct Output;

// Owns the ext-session-lock lifecycle for the daemon's lockscreen frame.
//
// Constructing this object does NOT lock the session: it only advertises the
// lockscreen as available (a frame with `lock = true` is declared and the
// compositor exposes ext-session-lock). `lock()` asks the compositor to lock;
// the actual surfaces are created when the compositor confirms with the
// `locked` event. Unlock happens on Escape/Return (via Display::on_key), on
// `CLI unlock` (SIGUSR2) or after successful authentication (PAM support =
// a later step).
class SessionLock {
public:
    SessionLock(Display& d, const ShellConfig& cfg, ModuleSupervisor& sup,
                const std::vector<Output>& outputs);
    ~SessionLock();

    // True when the lockscreen can actually lock: the compositor exposes
    // ext-session-lock AND a frame with `lock = true` is in the config.
    bool available() const { return spec_.has_value() && d_.session_lock; }

    bool locked() const { return locked_; }

    // True while a lock request is in flight (lock object created, but the
    // compositor has not confirmed with `locked` yet). The session may be
    // frozen but not accredited: `abort()` must be the recovery path.
    bool pending() const { return lock_ && !locked_; }

    // Ask the compositor to lock the session (no-op while already locking).
    // Returns false immediately if the lock surfaces could not be prepared
    // (in which case the request is rolled back — the session is NOT left
    // locked with a dead lockscreen).
    bool lock();

    // Unlock the session (Escape/Return, or CLI unlock). Only valid while
    // the compositor confirmed `locked` (otherwise use `abort`).
    void unlock();

    // Cancel a pending (not yet confirmed) lock: destroy the lock object so
    // the compositor releases the session. Escape/Return also take this path
    // if the transition is still in flight.
    void abort();

    // The lock surfaces (one per output), valid once the compositor confirmed
    // the lock; drawn by the daemon's repaint loop like the other frames.
    const std::vector<std::unique_ptr<Frame>>& frames() const { return frames_; }

    // Protocol callbacks (registered as ext_session_lock listeners).
    void on_locked();
    void on_finished();

private:
    void on_key(uint32_t keycode, uint32_t state);
    // Close the lock object following the protocol rules (destroy vs
    // unlock_and_destroy depending on whether `locked` was received) and
    // drop the lock surfaces.
    void teardown(bool received_locked);

    Display& d_;
    const ShellConfig& cfg_;
    ModuleSupervisor& sup_;
    const std::vector<Output>& outputs_;
    std::optional<FrameSpec> spec_;  // the declared `lock = true` frame

    ext_session_lock_v1* lock_{nullptr};
    std::vector<std::unique_ptr<Frame>> frames_;
    bool locked_{false};
};

}  // namespace otaku