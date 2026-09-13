#include "otaku/session_lock.hpp"

#include <cstdio>

#include "otaku/frame.hpp"
#include "otaku/module.hpp"
#include "otaku/supervisor.hpp"
#include "otaku/wayland.hpp"

namespace otaku {

namespace {

// XKB keycodes used to unlock without a password (PAM auth is a later step).
constexpr uint32_t kKeyEscape = 9;
constexpr uint32_t kKeyReturn = 36;

void lock_locked(void* data, ext_session_lock_v1*) {
    static_cast<SessionLock*>(data)->on_locked();
}

void lock_finished(void* data, ext_session_lock_v1*) {
    static_cast<SessionLock*>(data)->on_finished();
}

constexpr ext_session_lock_v1_listener g_lock_listener = {lock_locked,
                                                          lock_finished};

}  // namespace

SessionLock::SessionLock(Display& d, const ShellConfig& cfg,
                         ModuleSupervisor& sup,
                         const std::vector<Output>& outputs)
    : d_(d), cfg_(cfg), sup_(sup), outputs_(outputs) {
    for (const auto& f : cfg.frames) {
        if (f.lock) {
            spec_ = f;
            break;
        }
    }
}

SessionLock::~SessionLock() {
    if (lock_) teardown(locked_);
}

bool SessionLock::lock() {
    if (!available() || lock_) return false;
    lock_ = ext_session_lock_manager_v1_lock(d_.session_lock);
    ext_session_lock_v1_add_listener(lock_, &g_lock_listener, this);
    d_.on_key = [this](uint32_t k, uint32_t s) { on_key(k, s); };
    std::printf("otakud: locking session (%s)\n", spec_->id.c_str());
    std::printf("  lock obj id=%u\n",
                static_cast<uint32_t>(wl_proxy_get_id(
                    reinterpret_cast<wl_proxy*>(lock_))));

    // The compositor emits `locked` only once a "locked" frame has been
    // presented on every output, so the lock surfaces must be created (and
    // committed) now — not after the locked event.
    for (const auto& out : outputs_) {
        auto surf = create_lock_surface(d_, out, lock_, spec_->id);
        auto f = std::make_unique<Frame>(*spec_);
        f->set_theme(cfg_.colors);
        f->attach_modules(sup_, cfg_);
        f->set_surface(std::move(surf));
        frames_.push_back(std::move(f));
    }
    wl_display_roundtrip(d_.display);  // configures -> first committed buffers
    for (auto& f : frames_) f->render();
    wl_display_flush(d_.display);

    // Safety: every lock surface must have been configured (real size) and
    // hold a buffer. If any is missing (e.g. shm allocation failed) the
    // compositor would freeze the session with no painted lockscreen and
    // never send `locked` — the "dead lock" trap. Roll the request back.
    if (frames_.empty()) {
        std::fprintf(stderr, "otakud: no outputs to lock, aborting lock\n");
        teardown(locked_);
        return false;
    }
    for (auto& f : frames_) {
        if (!f->ready()) {
            std::fprintf(stderr,
                         "otakud: lock surface '%s' not ready, aborting lock\n",
                         f->id().c_str());
            teardown(locked_);
            return false;
        }
    }
    std::printf("otakud: lock surfaces presented on %zu output(s)\n",
                frames_.size());
    return true;
}

void SessionLock::unlock() {
    if (!lock_ || !locked_) return;
    std::printf("otakud: unlocking session\n");
    // unlock_and_destroy also frees the lock object server-side.
    ext_session_lock_v1_unlock_and_destroy(lock_);
    lock_ = nullptr;
    locked_ = false;
    frames_.clear();
    d_.on_key = {};
    wl_display_flush(d_.display);
}

void SessionLock::abort() {
    if (!lock_ || locked_) return;
    std::printf("otakud: canceling pending session lock\n");
    // No `locked` event was received: the protocol requires `destroy` here
    // (calling unlock_and_destroy would be a protocol error). Destroying the
    // lock object makes the compositor release the session.
    teardown(/*received_locked=*/false);
}

void SessionLock::on_locked() {
    locked_ = true;
    std::printf("otakud: session locked\n");
    wl_display_flush(d_.display);

    if (outputs_.empty()) {
        std::fprintf(stderr,
                     "otakud: warning: no outputs, locking with no surfaces\n");
    }
}

void SessionLock::on_finished() {
    // The compositor declined or closed the lock (e.g. another lock client
    // took over). Close the object and stay usable for a future lock().
    const bool had_locked = locked_;
    std::printf("otakud: session lock finished\n");
    teardown(had_locked);
}

void SessionLock::teardown(bool received_locked) {
    if (received_locked) {
        ext_session_lock_v1_unlock_and_destroy(lock_);
    } else if (lock_) {
        ext_session_lock_v1_destroy(lock_);
    }
    lock_ = nullptr;
    locked_ = false;
    frames_.clear();
    d_.on_key = {};
}

void SessionLock::on_key(uint32_t keycode, uint32_t state) {
    if (!lock_ || state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (keycode != kKeyEscape && keycode != kKeyReturn) return;
    if (locked_) {
        unlock();
    } else {
        // Mid-transition: still recoverable without a confirmed lock.
        abort();
    }
}

}  // namespace otaku