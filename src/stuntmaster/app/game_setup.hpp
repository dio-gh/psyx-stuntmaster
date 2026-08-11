#pragma once

#include "options.hpp"

namespace stuntmaster::app {

// Interactive first-launch flow, used when needsInteractiveGameSetup() is true.
// Prompts for the folder containing the supported BIN/CUE dump, validates the
// disc image, and persists the selection to the per-user config. On success
// sets options.game and options.run_live and returns true. Returns false only
// when the user chooses to quit instead of selecting a game.
//
// Win32-only; on other platforms it returns false (no game selected).
[[nodiscard]] bool resolveGameInteractively(Options& options);

} // namespace stuntmaster::app
