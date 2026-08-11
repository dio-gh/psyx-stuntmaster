#pragma once

#include "stuntmaster/game/guest_schedule.hpp"
#include "stuntmaster/psx/r3000_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace stuntmaster::core {
class StateReader;
class StateWriter;
}

namespace stuntmaster::disc {
class Iso9660Image;
}

namespace stuntmaster::game {

enum class RetailHleStatus {
    not_boundary,
    handled,
    memory_fault,
};

struct RetailHleResult {
    RetailHleStatus status{RetailHleStatus::not_boundary};
    std::uint32_t address{};
};

struct RetailMoviePlayRequest {
    std::string path;
    bool followed_by_movie{};
};

enum class HostMenuCommand : std::uint8_t {
    guest_update_rate,
    render_size,
    widescreen_cull,
    show_licenses,
};

struct HostMenuEvent {
    HostMenuCommand command{};
    std::uint32_t value{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct HostRenderSize {
    std::uint32_t width{};
    std::uint32_t height{};

    bool operator==(const HostRenderSize&) const = default;
};

inline constexpr std::size_t host_render_size_count = 4U;

inline constexpr std::uint32_t console_cd_read_speed = 2U;
inline constexpr std::uint32_t accelerated_load_tail_start_frames = 30U;

// Keep accelerated CD completion through bulk loading, then return to the
// console drive rate during a load's final upload-quiet window. Uploads
// resuming or the guest publishing a steady gameplay state restores the
// requested speed. This heuristic controls CD pacing only, never guest cadence.
[[nodiscard]] constexpr std::uint32_t cdReadSpeedForLoadPhase(
    std::uint32_t requested_speed,
    std::uint32_t frames_since_upload,
    std::uint32_t upload_quiet_frames) noexcept {
    return requested_speed > console_cd_read_speed &&
            frames_since_upload >= accelerated_load_tail_start_frames &&
            frames_since_upload < upload_quiet_frames
        ? console_cd_read_speed
        : requested_speed;
}

// Verified, unconditional consecutive Game::PlayMovie calls in the supported
// executable. Frames produced by the first call's display teardown are
// transitional, not a new game screen.
[[nodiscard]] constexpr bool retailMovieCallerChainsAnother(
    std::uint32_t return_address) noexcept {
    return return_address == 0x800C9A18U ||
        return_address == 0x800C9A34U ||
        return_address == 0x8002B968U;
}

// Game-revision-specific host implementations of Psy-Q/platform functions.
// These functions may update library-owned guest data but never gameplay state.
class RetailHle final {
public:
    static constexpr std::size_t vsync_callback_slot_count = 8U;
    using CdReadSink = std::function<void(
        std::uint32_t lba,
        std::uint32_t sector_count,
        std::uint32_t destination,
        std::uint32_t return_address)>;
    using MoviePrepareSink =
        std::function<void(const RetailMoviePlayRequest&)>;
    using MoviePlaySink = std::function<void(const RetailMoviePlayRequest&)>;
    using MovieTransitionSink = std::function<void()>;
    using HostMenuSink = std::function<void(const HostMenuEvent&)>;

    RetailHle() = default;
    explicit RetailHle(disc::Iso9660Image& image) noexcept : image_(&image) {}

    [[nodiscard]] static std::span<const std::uint32_t>
    executionBoundaries() noexcept;
    // Collapse verified, side-effect-free failed polls in retail's
    // WaitForLayer loop. Returns the number of 42-instruction polls charged;
    // zero means the exact CPU/object state was not safe to accelerate.
    [[nodiscard]] static std::uint64_t fastForwardWaitForLayerPolls(
        psx::R3000Runtime& runtime,
        std::uint64_t maximum_polls);
    [[nodiscard]] RetailHleResult dispatch(psx::R3000Runtime& runtime);
    [[nodiscard]] bool onVBlank(psx::R3000Runtime& runtime);
    void setPadOneState(
        bool connected, std::uint16_t active_low_buttons = 0xFFFFU) noexcept {
        pad_one_connected_ = connected;
        pad_one_buttons_ = active_low_buttons;
    }
    void setCdReadSink(CdReadSink sink) {
        cd_read_sink_ = std::move(sink);
    }
    void setMoviePlaySink(MoviePlaySink sink) {
        movie_play_sink_ = std::move(sink);
    }
    void setMoviePrepareSink(MoviePrepareSink sink) {
        movie_prepare_sink_ = std::move(sink);
    }
    void setMovieTransitionSink(MovieTransitionSink sink) {
        movie_transition_sink_ = std::move(sink);
    }
    // Opt-in experiment: overlay FE_MNU.TXT/FE.1 as sectors are copied from
    // the fingerprinted disc, then route three retail selectors through a
    // reserved HLE callback. The guest continues to own menu navigation and
    // rendering; the callback publishes host configuration requests only.
    void enableExperimentalHostMenu();
    void setHostMenuSink(HostMenuSink sink) {
        host_menu_sink_ = std::move(sink);
    }
    void setHostMenuState(
        std::uint32_t guest_update_rate,
        HostRenderSize render_size,
        bool widescreen_cull) noexcept;
    void setHostMenuUpdateRate(std::uint32_t guest_update_rate) noexcept {
        host_menu_update_rate_ = guest_update_rate >= 60U ? 60U : 30U;
    }
    void setHostMenuWidescreenCull(bool enabled) noexcept {
        host_menu_widescreen_cull_ = enabled;
        host_menu_wide_render_sizes_ = enabled;
    }
    void setHostMenuRenderSizeFamilies(
        std::array<HostRenderSize, host_render_size_count> narrow_sizes,
        std::array<HostRenderSize, host_render_size_count> wide_sizes) noexcept {
        host_narrow_render_sizes_ = narrow_sizes;
        host_wide_render_sizes_ = wide_sizes;
    }
    [[nodiscard]] HostRenderSize hostMenuRenderSizeForWidescreen(
        bool widescreen) const noexcept {
        const auto& sizes = widescreen
            ? host_wide_render_sizes_
            : host_narrow_render_sizes_;
        return sizes[std::min<std::size_t>(
            host_menu_render_size_index_, sizes.size() - 1U)];
    }
    // Keeps the memory-only bridge independently testable without requiring
    // retail disc assets. Production enables it through the asset overlay
    // method above.
    void setExperimentalHostMenuEnabled(bool enabled) noexcept {
        host_menu_enabled_ = enabled;
    }
    [[nodiscard]] std::uint32_t currentCdLba() const noexcept {
        return current_cd_lba_;
    }
    [[nodiscard]] std::uint32_t cdStatus() const noexcept { return cd_status_; }
    [[nodiscard]] std::uint64_t cdReadCalls() const noexcept {
        return cd_read_calls_;
    }
    // Pace CD reads at the drive's real transfer rate instead of completing
    // them the instant they are issued. Retail's loader at 0x8007EF20 polls
    // `CdReadSync(1, 0)` and, on a positive remainder, returns state 5 and
    // comes back -- so a remaining count is exactly what it is built to
    // handle. Data still lands in guest RAM immediately; only the completion
    // is delayed.
    // The drive-speed multiple: 1 is a single-speed 75 sectors a second, 2 the
    // double-speed drive a PlayStation actually has, and higher values trade
    // fidelity for a shorter wait. The application scheduler may temporarily
    // lower a fast rate during its upload-quiet load tail. Zero disables pacing
    // entirely.
    void setCdReadSpeed(std::uint32_t multiple) noexcept {
        cd_read_speed_ = multiple;
    }
    [[nodiscard]] std::uint32_t cdReadSpeed() const noexcept {
        return cd_read_speed_;
    }
    // The emulated display rate the completion drain is measured against. The
    // drive's transfer rate is a wall-clock quantity, so a faster emulated
    // VBlank has to drain proportionally less per VBlank or a high-frequency
    // guest would see the disc speed up with it.
    void setVblankRate(std::uint32_t rate) noexcept {
        vblank_rate_ = rate == 0U ? console_vblank_rate : rate;
    }
    [[nodiscard]] std::uint32_t vblankRate() const noexcept {
        return vblank_rate_;
    }
    // Sets the outstanding transfer without a disc behind it. The completion
    // model converts a wall-clock drive rate into the emulated VBlank rate,
    // which is worth exercising on its own.
    void setCdPendingSectors(std::uint64_t sectors) noexcept {
        cd_pending_units_ = sectors * cd_units_per_sector;
    }
    [[nodiscard]] std::uint64_t cdSectorsPending() const noexcept {
        return (cd_pending_units_ + cd_units_per_sector - 1U) /
            cd_units_per_sector;
    }
    [[nodiscard]] std::uint64_t cdSectorsRead() const noexcept {
        return cd_sectors_read_;
    }
    [[nodiscard]] std::uint64_t vblankCount() const noexcept {
        return vblank_count_;
    }
    [[nodiscard]] std::uint32_t vsyncCallback() const noexcept {
        return vsync_callbacks_[4U];
    }
    [[nodiscard]] const std::array<
        std::uint32_t, vsync_callback_slot_count>&
    vsyncCallbacks() const noexcept {
        return vsync_callbacks_;
    }
    void writeState(core::StateWriter& writer) const;
    [[nodiscard]] bool readState(core::StateReader& reader);

private:
    [[nodiscard]] RetailHleResult dispatchCdInit(psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult dispatchCdControl(
        psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult dispatchCdReady(psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult dispatchCdRead(psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult dispatchCdReadSync(
        psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult dispatchCdStatus(psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult dispatchCdSync(psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult dispatchDrawSync(
        psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult dispatchVSync(psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult dispatchVSyncCallback(
        psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult dispatchVSyncCallbacks(
        psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult dispatchMoviePlay(
        psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult captureMoviePath(
        psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult dispatchHostMenuCallback(
        psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult inspectHostMenuPush(
        psx::R3000Runtime& runtime);
    // Splice a native, game-rendered "LICENSES" row into the pause MAIN MENU
    // (Menu_Title). Relocates the menu's text-prim overlay into the reserved
    // menu-object arena with one extra slot, clones a title text object for the
    // new label, and repoints the section/overlay-inventory and owning
    // xcScreen so the retail renderer draws it. Idempotent per menu instance.
    [[nodiscard]] bool ensureLicensesMenuItem(
        psx::R3000Runtime& runtime, std::uint32_t manager);
    // Pre-hook on MenuMgr::SetTopMenu: injects the Licenses row as the pause
    // menu's root is established, so it is present on the first pause-open.
    [[nodiscard]] RetailHleResult injectLicensesOnSetTopMenu(
        psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult aliasHostMenuScreen(
        psx::R3000Runtime& runtime);
    [[nodiscard]] RetailHleResult restoreHostMenuId(
        psx::R3000Runtime& runtime);
    [[nodiscard]] const std::array<
        HostRenderSize, host_render_size_count>&
    activeHostRenderSizes() const noexcept {
        return host_menu_wide_render_sizes_
            ? host_wide_render_sizes_
            : host_narrow_render_sizes_;
    }

    struct CdFileOverride {
        std::uint32_t extent_lba{};
        std::vector<std::byte> bytes;
    };
    void addCdFileOverride(
        const std::string& path,
        std::vector<std::byte> bytes);
    void applyCdFileOverrides(
        std::uint32_t first_lba,
        std::span<std::byte> sectors) const;

    std::uint32_t video_timing_baseline_{};
    std::uint32_t current_cd_lba_{};
    std::uint32_t cd_status_{};
    std::uint64_t cd_read_calls_{};
    std::uint64_t cd_sectors_read_{};
    // A single-speed drive reads 75 sectors a second. Counting in
    // quarter-sectors keeps every speed multiple exact in integers: four units
    // to a sector, 300 units a second per multiple. At the console's 60 VBlanks
    // that is five units a VBlank per multiple, and double speed — the drive a
    // PlayStation actually has — drains ten.
    //
    // A faster emulated VBlank does not divide 300 evenly, so the leftover is
    // carried in `cd_drain_remainder_` rather than dropped. The drive therefore
    // delivers the same sectors per wall-clock second at every VBlank rate.
    static constexpr std::uint64_t cd_units_per_sector = 4U;
    static constexpr std::uint64_t cd_units_per_second_per_speed = 300U;
    std::uint64_t cd_pending_units_{};
    std::uint64_t cd_drain_remainder_{};
    std::uint32_t cd_read_speed_{};
    std::uint32_t vblank_rate_{console_vblank_rate};
    std::uint64_t vblank_count_{};
    std::array<std::uint32_t, vsync_callback_slot_count> vsync_callbacks_{};
    std::uint16_t pad_one_buttons_{0xFFFFU};
    bool pad_one_connected_{true};
    bool video_timing_baseline_initialized_{};
    disc::Iso9660Image* image_{};
    CdReadSink cd_read_sink_;
    MovieTransitionSink movie_transition_sink_;
    MoviePrepareSink movie_prepare_sink_;
    MoviePlaySink movie_play_sink_;
    HostMenuSink host_menu_sink_;
    std::string pending_movie_path_;
    bool pending_movie_followed_by_movie_{};
    std::vector<CdFileOverride> cd_file_overrides_;
    std::array<HostRenderSize, host_render_size_count>
        host_narrow_render_sizes_{{
            {640U, 480U}, {960U, 720U}, {1440U, 1080U}, {1920U, 1440U}}};
    std::array<HostRenderSize, host_render_size_count>
        host_wide_render_sizes_{{
            {854U, 480U}, {1280U, 720U}, {1920U, 1080U}, {2560U, 1440U}}};
    std::uint32_t host_menu_update_rate_{30U};
    std::uint32_t host_menu_render_size_index_{1U};
    std::array<std::uint32_t, 8U> host_menu_volume_style_{};
    bool host_menu_volume_style_valid_{};
    bool host_menu_widescreen_cull_{};
    bool host_menu_wide_render_sizes_{true};
    bool host_menu_enabled_{};
};

} // namespace stuntmaster::game
