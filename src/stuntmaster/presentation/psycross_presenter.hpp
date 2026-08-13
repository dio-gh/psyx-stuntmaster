#pragma once

#include "stuntmaster/presentation/debug_overlay.hpp"
#include "stuntmaster/presentation/license_overlay.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Matches SDL's own global typedef; keeps SDL.h out of this header.
typedef struct _SDL_GameController SDL_GameController;

namespace stuntmaster::presentation {

struct GpuReplaySegment {
    std::vector<std::vector<std::uint32_t>> packets;
    std::vector<std::uint16_t> vram;
    std::uint64_t vram_revision{};
};

// Presentation adapter for captured retail GPU state. It is built only when
// STUNTMASTER_ENABLE_PSYCROSS is enabled.
class PsyCrossPresenter final {
public:
    explicit PsyCrossPresenter(
        const std::filesystem::path& input_config = {},
        std::uint32_t window_width = 1280U,
        std::uint32_t window_height = 720U,
        std::uint32_t render_width = 1280U,
        std::uint32_t render_height = 720U,
        bool capture_frame_trace = false,
        // Create the window hidden so the live present path can be driven and
        // read back headlessly (Stage 0 of the presentation redesign). The GL
        // context and every offscreen framebuffer are still created.
        bool hidden_window = false,
        // Start in SDL borderless-desktop fullscreen (SDL_WINDOW_FULLSCREEN_-
        // DESKTOP). Alt+Enter still toggles it at runtime (handled inside
        // PsyCross); the current state is readable via isFullscreen().
        bool fullscreen = false);
    ~PsyCrossPresenter();

    PsyCrossPresenter(const PsyCrossPresenter&) = delete;
    PsyCrossPresenter& operator=(const PsyCrossPresenter&) = delete;

    [[nodiscard]] std::uint16_t pollPadOneButtons();
    // Drive the first attached SDL game controller's rumble motors. `motor1`
    // is the DualShock big/low-frequency actuator and `motor2` the
    // small/high-frequency one, matching the retail actuator table order.
    // `duration_ms` is the guest shake duration; zero stops the rumble.
    // Main thread only (SDL controller access).
    void applyRumble(
        std::uint8_t motor1,
        std::uint8_t motor2,
        std::uint32_t duration_ms);
    [[nodiscard]] bool takeQuickSaveRequest() noexcept {
        return std::exchange(quick_save_requested_, false);
    }
    [[nodiscard]] bool takeQuickLoadRequest() noexcept {
        return std::exchange(quick_load_requested_, false);
    }
    [[nodiscard]] bool takeTimestampedQuickSaveRequest() noexcept {
        return std::exchange(timestamped_quick_save_requested_, false);
    }
    [[nodiscard]] bool takeRetimeToggleRequest() noexcept {
        return std::exchange(retime_toggle_requested_, false);
    }
    [[nodiscard]] bool takeWidescreenToggleRequest() noexcept {
        return std::exchange(widescreen_toggle_requested_, false);
    }
    void setDebugOverlay(DebugOverlayState state) noexcept;
    [[nodiscard]] std::uint32_t displayRefreshRate() const noexcept {
        return display_refresh_rate_;
    }
    // Whether the window is currently in SDL borderless-desktop fullscreen,
    // read from the live SDL window flags. The user can toggle this at runtime
    // with Alt+Enter (handled inside PsyCross), so the application polls this to
    // persist the choice.
    [[nodiscard]] bool isFullscreen() const noexcept;
    // Invoked on the polling thread the instant the window's fullscreen state
    // changes -- including a toggle made during an FMV, which pollPadOneButtons
    // still drives. The application uses this to persist the choice immediately,
    // because a window close hard-exits (PsyCross calls exit(0) from its event
    // pump) and never returns to the main loop's persistence pass.
    void setFullscreenChangedCallback(std::function<void(bool)> callback) {
        fullscreen_changed_callback_ = std::move(callback);
    }
    // The current windowed client size in pixels. While borderless fullscreen
    // is active this reports the last size the window had before going
    // fullscreen, so the application persists the windowed size the user chose
    // rather than the monitor-filling fullscreen extent.
    [[nodiscard]] std::uint32_t windowedWidth() const noexcept {
        return last_windowed_width_;
    }
    [[nodiscard]] std::uint32_t windowedHeight() const noexcept {
        return last_windowed_height_;
    }
    void showNotification(std::string message);

    // Host license viewer (hybrid P5(B)): a pageable/scrollable overlay drawn on
    // top of the frame. Documents are supplied once; the viewer is opened by the
    // in-game menu trigger (or the host 'L' key) and navigated with the pad/arrows.
    void setLicenseDocuments(std::vector<LicenseDocument> documents);
    void openLicenseViewer();
    void closeLicenseViewer() noexcept;
    [[nodiscard]] bool isLicenseViewerActive() const noexcept {
        return license_viewer_active_;
    }
    // Recreate PsyCross's internal target on the next present while retaining
    // the independently resizable SDL window and input/audio ownership.
    void setRenderSize(std::uint32_t width, std::uint32_t height);
    void repeatFrame();

    // Replace any retained guest image with a host-owned black transition
    // frame. Used while native movie decoding is taking ownership of the
    // window, before its first decoded frame is available.
    void presentBlackFrame();

    // Present a decoded native RGBA movie frame through the same main-thread
    // window/context as gameplay. Rows are top-down, matching movie output.
    void presentMovieFrame(
        std::span<const std::uint8_t> rgba8888,
        std::uint32_t width,
        std::uint32_t height);

    // Show the frame already held, without going back through PsyCross's
    // cached copy. Most presentations are repeats at a high presentation rate
    // against a 30 Hz guest, so this keeps the Psy-Q layer out of the majority
    // of what reaches the screen.
    void repeatScanout();

    // Draw the selected PS1 display page for screens retail builds by DMA.
    // A substantial world-polygon pass suppresses this base layer so upscale
    // cracks expose black rather than stale framebuffer imagery.
    void setCompositeDisplayPage(bool enable) noexcept {
        composite_display_page_ = enable;
    }

    // Drive PsyX as a display controller over a persistent framebuffer. This
    // does NOT clear the render target or rebuild it from the packet range each
    // call. The render target persists across presents;
    // primitives are drawn onto it in stream order, the guest's own 0x02 fills
    // clear where it asks, and screens retail *writes* into the display page
    // (CPU uploads and page copies) are composited into the render target from
    // VRAM in the same order. One rule replaces the composite/suppression
    // heuristics: whatever writes the display page appears, in order.
    //
    // Headless-first: the publication-dump harness presents every publication
    // in order single-threaded, so the model is validated against the Stage-0
    // capture before any live/threading work. See docs/PRESENTATION_REDESIGN.md.
    void presentPersistent(
        std::span<const std::uint16_t> vram,
        const std::vector<std::vector<std::uint32_t>>& packets,
        std::uint32_t display_x,
        std::uint32_t display_y,
        std::uint32_t display_width,
        std::uint32_t display_height,
        std::span<const GpuReplaySegment> segments = {});
    [[noreturn]] void showUntilClosed(
        std::span<const std::uint16_t> vram,
        const std::vector<std::vector<std::uint32_t>>& packets,
        std::uint32_t display_x,
        std::uint32_t display_y,
        std::uint32_t display_width,
        std::uint32_t display_height);
    void captureScreenshot(
        std::span<const std::uint16_t> vram,
        const std::vector<std::vector<std::uint32_t>>& packets,
        std::uint32_t display_x,
        std::uint32_t display_y,
        std::uint32_t display_width,
        std::uint32_t display_height);

    // Arm a one-shot capture: the next presentPersistent reads its composed
    // render target back to `destination` as a 32-bit bottom-up BMP, taken
    // *before* the window swap (the render target is not readable afterwards).
    // This is the faithful, headless-safe capture -- the same pixels the window
    // would show, through the same live present path, independent of window
    // visibility. The path is consumed by the present it triggers.
    void captureNextPresent(const std::filesystem::path& destination) {
        render_target_capture_path_ = destination;
    }

    // Low-level render-target readback + BMP write. Public for the Stage-0
    // harness; must be called with a live present's render target still bound
    // (i.e. from inside presentPersistent, before the swap) -- see
    // captureNextPresent.
    void captureRenderTarget(const std::filesystem::path& destination);

private:
    struct DiagnosticFrame {
        std::vector<unsigned char> bgra;
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint16_t> vram;
        std::vector<std::vector<std::uint32_t>> packets;
        std::uint32_t display_x{};
        std::uint32_t display_y{};
        std::uint32_t display_width{};
        std::uint32_t display_height{};
    };

    void captureDiagnosticFrame(
        std::span<const std::uint16_t> vram,
        const std::vector<std::vector<std::uint32_t>>& packets,
        std::uint32_t display_x,
        std::uint32_t display_y,
        std::uint32_t display_width,
        std::uint32_t display_height);
    void dumpDiagnosticFrames();

    bool initialized_{};
    bool capture_frame_trace_{};
    bool composite_display_page_{};
    // One-shot destination for the Stage-0 render-target capture, honoured and
    // cleared by the next present(). Empty means no capture pending.
    std::filesystem::path render_target_capture_path_{};
    // BGRA window-image scratch filled by the final window-buffer swap when a
    // capture is armed.
    std::vector<unsigned char> window_capture_buffer_{};
    void blitScanout();
    void finishWindowPresentation();
    void drawDebugOverlay();
    void drawNotificationOverlay();
    void drawLicenseOverlay();
    // Consume pad + keyboard navigation while the license viewer is open. The
    // pad word is active-low (a cleared bit means pressed).
    void updateLicenseViewerInput(std::uint16_t pad_buttons);

    bool has_scanout_{};
    std::uint32_t scanout_width_{};
    std::uint32_t scanout_height_{};
    unsigned int scanout_texture_{};
    unsigned int scanout_framebuffer_{};
    unsigned int debug_overlay_texture_{};
    unsigned int debug_overlay_framebuffer_{};
    unsigned int notification_overlay_texture_{};
    unsigned int notification_overlay_framebuffer_{};
    unsigned int license_overlay_texture_{};
    unsigned int license_overlay_framebuffer_{};
    LicenseOverlay license_overlay_;
    bool license_viewer_active_{};
    int license_toggle_key_{};
    bool license_toggle_key_down_{};
    // Key/button edge latches for the license viewer's discrete actions (page,
    // document change, close); continuous scroll needs no latch.
    bool license_page_up_down_{};
    bool license_page_down_down_{};
    bool license_prev_doc_down_{};
    bool license_next_doc_down_{};
    bool license_close_down_{};
    // Scroll auto-repeat: a fresh press moves one notch immediately; holding
    // repeats on a wall-clock schedule so the speed is independent of how often
    // input is polled (which varies between gameplay and FMV playback). -1 up,
    // +1 down, 0 released.
    int license_scroll_direction_{};
    std::chrono::steady_clock::time_point license_scroll_next_repeat_{};
    DebugOverlayState debug_overlay_{};
    DebugOverlayVisibilityToggle debug_overlay_toggle_{};
    bool debug_overlay_visible_{};
    int debug_overlay_toggle_key_{};
    int quick_save_key_{};
    int quick_load_key_{};
    int timestamped_quick_save_key_{};
    int retime_toggle_key_{};
    int widescreen_toggle_key_{};
    bool quick_save_key_down_{};
    bool quick_load_key_down_{};
    bool timestamped_quick_save_key_down_{};
    bool retime_toggle_key_down_{};
    bool widescreen_toggle_key_down_{};
    bool quick_save_requested_{};
    bool quick_load_requested_{};
    bool timestamped_quick_save_requested_{};
    bool retime_toggle_requested_{};
    bool widescreen_toggle_requested_{};
    std::string notification_message_;
    std::chrono::steady_clock::time_point notification_expires_{};
    std::uint32_t window_width_{};
    std::uint32_t window_height_{};
    std::uint32_t display_refresh_rate_{60U};
    std::uint32_t render_width_{};
    std::uint32_t render_height_{};
    std::uint16_t previous_trace_buttons_{0xFFFFU};
    std::uint32_t capture_trace_event_{};
    std::deque<DiagnosticFrame> diagnostic_frames_;
    // The last client size the window had while not in borderless fullscreen.
    // Updated every input poll so a runtime resize is captured, and left frozen
    // while fullscreen so it keeps reporting the user's chosen windowed size.
    std::uint32_t last_windowed_width_{};
    std::uint32_t last_windowed_height_{};
    // The live (on-screen) presenter drives its window swaps at vsync so the
    // borderless-desktop scanout is tear-free. False for the headless capture
    // path, which must present uncapped. See finishWindowPresentation().
    bool vsync_wanted_{};
    // Last-seen fullscreen state, so pollPadOneButtons fires the change callback
    // exactly once per edge.
    bool last_fullscreen_state_{};
    // Fired on every fullscreen-state edge so the app can persist immediately.
    std::function<void(bool)> fullscreen_changed_callback_{};
    std::array<unsigned char, 34> pad_one_{};
    std::array<unsigned char, 34> pad_two_{};
    SDL_GameController* rumble_controller_{};
    bool rumble_active_{};
};

} // namespace stuntmaster::presentation
