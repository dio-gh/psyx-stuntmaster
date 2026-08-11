#pragma once

namespace stuntmaster::app {

enum class RunConsoleMode {
    // CLI / probe run: keep stdout as-is. Either it is already redirected (a
    // pipe or file, e.g. CI capturing a probe) or we attach to the parent
    // console so a terminal launch stays visible.
    attach_or_redirect,
    // Ordinary double-click with no arguments: no console window; diagnostics
    // go to the per-user log file.
    log_file,
};

// Decide how stdio should behave. Pure and testable: any command-line argument,
// or an already-redirected stdout, means "keep stdout"; a bare launch logs to
// a file.
[[nodiscard]] RunConsoleMode chooseConsoleMode(
    int argc, bool stdout_redirected) noexcept;

// Apply the console/logging decision for this process. On a bare GUI launch this
// redirects stdout/stderr to <Documents>\Stuntmaster\logs\stuntmaster.log; with
// arguments it keeps an existing redirection or attaches the parent console.
// Windows-only; a no-op elsewhere. Call once, first thing in main().
void configureConsoleAndLogging(int argc);

} // namespace stuntmaster::app
