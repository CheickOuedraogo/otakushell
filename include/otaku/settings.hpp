#pragma once

namespace otaku {

// Run the graphical settings window (`otakushell settings`). Opens a toplevel
// window drawn with cairo, edits a working copy of config.toml and applies
// every change by rewriting the config and signaling the daemon (SIGUSR1).
// Blocks until the window is closed. Returns a process exit code.
int run_settings();

}  // namespace otaku