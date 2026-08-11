#include "stuntmaster/game/retail_hle.hpp"

#include "stuntmaster/core/state_archive.hpp"
#include "stuntmaster/core/error.hpp"
#include "stuntmaster/disc/iso9660.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <string_view>

namespace stuntmaster::game {
namespace {

constexpr std::uint32_t vsync_address = 0x800366E8U;
constexpr std::uint32_t vsync_callback_address = 0x8002CEB4U;
constexpr std::uint32_t vsync_callbacks_address = 0x8002CEE8U;
constexpr std::uint32_t draw_sync_address = 0x80026C04U;
constexpr std::uint32_t vcount_address = 0x800D8974U;
constexpr std::uint32_t cd_init_address = 0x800C746CU;
constexpr std::uint32_t cd_control_address = 0x800C6E50U;
constexpr std::uint32_t cd_ready_address = 0x800C6B88U;
constexpr std::uint32_t cd_read_address = 0x800B8724U;
constexpr std::uint32_t cd_read_sync_address = 0x800B88C0U;
constexpr std::uint32_t cd_status_address = 0x800B89C4U;
constexpr std::uint32_t cd_sync_address = 0x800C6908U;
constexpr std::uint32_t movie_play_address = 0x80014534U;
constexpr std::uint32_t game_play_movie_address = 0x8002BBF0U;
constexpr std::uint32_t game_play_movie_return_address = 0x8002BD0CU;
// The title Start route reaches this `jal FadeEnd` only after FadeUpdate has
// clamped to black and returned false. Front-end teardown/loading begins
// immediately afterward, before Game::PlayMovie is entered.
constexpr std::uint32_t title_movie_fade_complete_address = 0x8002C178U;
constexpr std::uint32_t pad_one_buffer_address = 0x800DFA30U;
constexpr std::uint32_t pad_two_buffer_address = 0x800DFA64U;
// The game's linked libpad copy keeps one 0xF0-byte pad struct per port in
// BSS (base published through the global at 0x800D89F0). Its pad-detection
// state machine runs in the SIO interrupt handler, which this host does not
// emulate, so the driver never advances past the BSS zero state byte. The
// host therefore publishes the fully-detected DualShock configuration the
// retail vibration checks expect: `PadGetState(0) == 6` (state byte +0x49,
// returned raw in the clean-stable condition) gates the Options vibration
// toggle, the shake function, and the shake countdown pump.
constexpr std::uint32_t pad_struct_port_zero_address = 0x800E28FCU;
// +0x46: driver mode byte, 0xFE marks detection complete (act info loaded).
constexpr std::uint32_t pad_struct_mode_offset = 0x46U;
// +0x49: pad state byte; 6 is the act-info-loaded, DualShock-ready state.
constexpr std::uint32_t pad_struct_state_offset = 0x49U;
// +0xE3/+0xE4: actuator-combination count and actuator count (PadInfoMode
// info 3/4), 1 and 2 for a DualShock.
constexpr std::uint32_t pad_struct_comb_count_offset = 0xE3U;
constexpr std::uint32_t pad_struct_actuator_count_offset = 0xE4U;
// +0xE6: mode-switch mask (PadInfoMode info 1), 0x73 for a DualShock in
// digital mode. +0xE8: current mode id, 0x41 (digital).
constexpr std::uint32_t pad_struct_mode_switch_mask_offset = 0xE6U;
constexpr std::uint32_t pad_struct_current_mode_offset = 0xE8U;
// +0xE9/+0xEA: per-combo actuator layout, one combo of two actuators.
constexpr std::uint32_t pad_struct_comb_entry_count_offset = 0xE9U;
constexpr std::uint32_t pad_struct_actuators_per_comb_offset = 0xEAU;
constexpr std::uint8_t pad_struct_ready_state = 6U;
constexpr std::uint8_t pad_struct_ready_mode = 0xFEU;
constexpr std::uint8_t pad_struct_comb_count = 1U;
constexpr std::uint8_t pad_struct_actuator_count = 2U;
constexpr std::uint16_t pad_struct_mode_switch_mask = 0x73U;
constexpr std::uint8_t pad_struct_current_mode = 0x41U;
constexpr std::uint8_t pad_struct_comb_entry_count = 1U;
constexpr std::uint8_t pad_struct_actuators_per_comb = 2U;
constexpr std::uint32_t host_menu_push_address = 0x80010D08U;
constexpr std::uint32_t host_menu_screen_alias_address = 0x80010E0CU;
constexpr std::uint32_t host_menu_id_restore_address = 0x80010E18U;
// MenuMgr::SetTopMenu(manager, menuHash). Establishes a manager's root menu;
// SelfInit__9feMenuMgr calls it for Menu_Title when the pause menu is built,
// before it is ever displayed. Hooking it here makes the injected "Licenses"
// row exist on the first pause-open, not only after a submenu round-trip.
constexpr std::uint32_t set_top_menu_address = 0x8005F7DCU;
// Free space between the BIOS-HLE device table and the retail-patch arena.
// No instruction is stored here: the boundary is consumed before fetch.
constexpr std::uint32_t host_menu_callback_address = 0x80002FF0U;
// Keep the logical menu ID distinct from the visible Display screen/object
// identity. Reusing `Display` here makes the front end take a stock screen
// path before the scoped Sound-screen alias can run, leaving the menu hidden.
// `HostDSP` is short enough for FE_MNU.TXT's fixed 811-byte retail extent.
constexpr std::uint32_t host_display_menu_hash = 0xB1AF7E45U;
constexpr std::uint32_t sound_menu_hash = 0x061CD029U;
constexpr std::uint32_t frame_rate_item_hash = 0x1B5DD3F5U;
constexpr std::uint32_t sound_music_item_hash = 0xB3DA1CE9U;
constexpr std::uint32_t resolution_item_hash = 0xB47983DEU;
constexpr std::uint32_t sound_stereo_item_hash = 0x3D030EFAU;
constexpr std::size_t iso_sector_size = 2048U;

// Pause MAIN MENU = Menu_Title. Native "LICENSES" row injection targets.
constexpr std::uint32_t menu_title_hash = 0x062B99E2U;
constexpr std::uint32_t hd_item_button_vtable = 0x800CD7A0U;
// Host-chosen identity for the injected item; distinct from every retail item
// hash, matched in dispatchHostMenuCallback to raise show_licenses.
constexpr std::uint32_t licenses_item_hash = 0x4C494345U; // 'LICE'
// Layout inside R3000Runtime::menu_object_arena_base (2 KB reserved data span).
// Overlay copy needs 8 + (nprims+1)*8 bytes; a title overlay holds ~6 prims.
constexpr std::uint32_t menu_arena_overlay = 0x80004000U;   // relocated overlay
constexpr std::uint32_t menu_arena_textobj = 0x80004080U;   // cloned xcTextObj
constexpr std::uint32_t menu_arena_button = 0x800040C0U;    // hdItemButton
constexpr std::uint32_t menu_arena_string = 0x800040E0U;    // "LICENSES\0"
// Row Y translation is guest screenY<<16 (~1 guest pixel = 0x10000). The six
// re-spaced rows keep the original top..bottom extent (so the bottom row lands
// where the stock bottom row sat, safely inside the frame) and are nudged down
// by a couple of pixels so the block is not top-heavy. Extra spread is avoided:
// it pushes the bottom row onto the frame border.
constexpr std::uint32_t menu_rows_down_bias = 0x00020000U;  // ~2 px lower
constexpr std::uint32_t menu_rows_extra_span = 0x00000000U; // keep stock extent

std::vector<std::byte> asBytes(std::string_view text, std::size_t size) {
    std::vector<std::byte> result(size);
    if (text.size() > result.size()) {
        throw core::Error{"experimental host menu exceeds retail file size"};
    }
    std::transform(text.begin(), text.end(), result.begin(), [](char value) {
        return std::byte{static_cast<unsigned char>(value)};
    });
    return result;
}

bool writeText(
    psx::R3000Runtime& runtime,
    std::uint32_t address,
    std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size() + 1U);
    for (const auto character : text) {
        bytes.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    bytes.push_back(std::byte{});
    return runtime.loadBytes(address, bytes);
}

bool decodeBcd(std::uint8_t value, std::uint32_t limit,
               std::uint32_t& decoded) noexcept {
    const auto high = static_cast<std::uint32_t>(value >> 4U);
    const auto low = static_cast<std::uint32_t>(value & 0x0FU);
    if (high > 9U || low > 9U) {
        return false;
    }
    decoded = high * 10U + low;
    return decoded < limit;
}

} // namespace

std::span<const std::uint32_t> RetailHle::executionBoundaries() noexcept {
    static constexpr std::array boundaries{
        host_menu_callback_address,
        host_menu_push_address,
        host_menu_screen_alias_address,
        host_menu_id_restore_address,
        set_top_menu_address,
        title_movie_fade_complete_address,
        game_play_movie_address,
        draw_sync_address,
        vsync_address,
        vsync_callback_address,
        vsync_callbacks_address,
        cd_init_address,
        cd_control_address,
        cd_ready_address,
        cd_read_address,
        cd_read_sync_address,
        cd_status_address,
        cd_sync_address,
        movie_play_address,
    };
    return boundaries;
}

void RetailHle::writeState(core::StateWriter& writer) const {
    writer.pod(video_timing_baseline_);
    writer.pod(current_cd_lba_);
    writer.pod(cd_status_);
    writer.pod(cd_read_calls_);
    writer.pod(cd_sectors_read_);
    writer.pod(cd_pending_units_);
    writer.pod(cd_drain_remainder_);
    writer.pod(cd_read_speed_);
    writer.pod(vblank_rate_);
    writer.pod(vblank_count_);
    writer.pod(vsync_callbacks_);
    writer.pod(pad_one_buttons_);
    writer.pod(pad_one_connected_);
    writer.pod(video_timing_baseline_initialized_);
    writer.string(pending_movie_path_);
    writer.pod(pending_movie_followed_by_movie_);
}

bool RetailHle::readState(core::StateReader& reader) {
    return reader.pod(video_timing_baseline_) &&
        reader.pod(current_cd_lba_) && reader.pod(cd_status_) &&
        reader.pod(cd_read_calls_) && reader.pod(cd_sectors_read_) &&
        reader.pod(cd_pending_units_) && reader.pod(cd_drain_remainder_) &&
        reader.pod(cd_read_speed_) && reader.pod(vblank_rate_) &&
        reader.pod(vblank_count_) && reader.pod(vsync_callbacks_) &&
        reader.pod(pad_one_buttons_) && reader.pod(pad_one_connected_) &&
        reader.pod(video_timing_baseline_initialized_) &&
        reader.string(pending_movie_path_, 1024U) &&
        reader.pod(pending_movie_followed_by_movie_);
}

std::uint64_t RetailHle::fastForwardWaitForLayerPolls(
    psx::R3000Runtime& runtime,
    std::uint64_t maximum_polls) {
    constexpr std::uint32_t wait_for_layer_poll_pc = 0x8009FDF0U;
    constexpr std::uint32_t view_address = 0x800DD780U;
    constexpr std::uint32_t single_layer_check = 0x800A03ACU;
    constexpr std::uint32_t double_layer_check = 0x800A05FCU;
    const auto& state = runtime.state();
    if (maximum_polls == 0U || state.pc != wait_for_layer_poll_pc ||
        state.gpr[31] != wait_for_layer_poll_pc || state.gpr[2] != 0U ||
        state.gpr[4] != 1U) {
        return 0U;
    }

    std::uint32_t view{};
    std::uint32_t layers{};
    std::uint32_t counters{};
    std::uint32_t counter{};
    std::uint32_t layer{};
    std::uint32_t layer_vtable{};
    std::uint32_t check_layer{};
    std::uint32_t layer_generation{};
    const auto layer_index = state.gpr[18];
    const auto counter_offset = state.gpr[16];
    const auto layer_slot = state.gpr[5];
    if (!runtime.read32(view_address, view) ||
        !runtime.read32(view + 0x10U, layers) ||
        layer_slot != layers + layer_index * sizeof(std::uint32_t) ||
        !runtime.read32(state.gpr[17] + 0x24U, counters) ||
        !runtime.read32(counters + counter_offset, counter) ||
        state.gpr[3] != counter || counter == 0U ||
        !runtime.read32(layer_slot, layer) ||
        !runtime.read32(layer + 0x2CU, layer_vtable) ||
        !runtime.read32(layer_vtable + 0x18U, check_layer) ||
        !runtime.read32(layer + 0x0CU, layer_generation) ||
        !((check_layer == single_layer_check && layer_generation == 1U) ||
          (check_layer == double_layer_check && layer_generation == 2U))) {
        return 0U;
    }

    const auto polls = std::min<std::uint64_t>(
        maximum_polls,
        std::numeric_limits<std::uint32_t>::max() - counter);
    if (polls == 0U) {
        return 0U;
    }
    const auto advanced_counter =
        counter + static_cast<std::uint32_t>(polls);
    if (!runtime.write32(counters + counter_offset, advanced_counter)) {
        return 0U;
    }
    runtime.setRegister(3U, advanced_counter);
    return polls;
}

RetailHleResult RetailHle::dispatch(psx::R3000Runtime& runtime) {
    if (runtime.state().pc == host_menu_callback_address) {
        return dispatchHostMenuCallback(runtime);
    }
    if (runtime.state().pc == host_menu_push_address) {
        return inspectHostMenuPush(runtime);
    }
    if (runtime.state().pc == host_menu_screen_alias_address) {
        return aliasHostMenuScreen(runtime);
    }
    if (runtime.state().pc == host_menu_id_restore_address) {
        return restoreHostMenuId(runtime);
    }
    if (runtime.state().pc == set_top_menu_address) {
        return injectLicensesOnSetTopMenu(runtime);
    }
    if (runtime.state().pc == title_movie_fade_complete_address) {
        if (movie_transition_sink_) {
            movie_transition_sink_();
        }
        return {};
    }
    if (runtime.state().pc == game_play_movie_address) {
        return captureMoviePath(runtime);
    }
    if (runtime.state().pc == draw_sync_address) {
        return dispatchDrawSync(runtime);
    }
    if (runtime.state().pc == vsync_address) {
        return dispatchVSync(runtime);
    }
    if (runtime.state().pc == vsync_callback_address) {
        return dispatchVSyncCallback(runtime);
    }
    if (runtime.state().pc == vsync_callbacks_address) {
        return dispatchVSyncCallbacks(runtime);
    }
    if (runtime.state().pc == cd_init_address) {
        return dispatchCdInit(runtime);
    }
    if (runtime.state().pc == cd_control_address) {
        return dispatchCdControl(runtime);
    }
    if (runtime.state().pc == cd_ready_address) {
        return dispatchCdReady(runtime);
    }
    if (runtime.state().pc == cd_read_address) {
        return dispatchCdRead(runtime);
    }
    if (runtime.state().pc == cd_read_sync_address) {
        return dispatchCdReadSync(runtime);
    }
    if (runtime.state().pc == cd_status_address) {
        return dispatchCdStatus(runtime);
    }
    if (runtime.state().pc == cd_sync_address) {
        return dispatchCdSync(runtime);
    }
    if (runtime.state().pc == movie_play_address &&
        runtime.state().gpr[31] == game_play_movie_return_address) {
        return dispatchMoviePlay(runtime);
    }
    return {};
}

RetailHleResult RetailHle::dispatchDrawSync(
    psx::R3000Runtime& runtime) {
    // DMA2 transfers and GP0 ingestion are synchronous in the bootstrap host.
    // Completion callbacks are scheduled separately by the host loop, so both
    // blocking and polling modes can report an idle GPU immediately.
    runtime.setRegister(2, 0U);
    runtime.completeHostCall();
    return {RetailHleStatus::handled, draw_sync_address};
}

RetailHleResult RetailHle::dispatchVSyncCallback(
    psx::R3000Runtime& runtime) {
    constexpr std::size_t standard_vsync_slot = 4U;
    const auto previous = vsync_callbacks_[standard_vsync_slot];
    vsync_callbacks_[standard_vsync_slot] = runtime.state().gpr[4];
    runtime.setRegister(2, previous);
    runtime.completeHostCall();
    return {RetailHleStatus::handled, vsync_callback_address};
}

RetailHleResult RetailHle::dispatchVSyncCallbacks(
    psx::R3000Runtime& runtime) {
    const auto slot = runtime.state().gpr[4];
    std::uint32_t previous{};
    if (slot < vsync_callbacks_.size()) {
        previous = vsync_callbacks_[slot];
        vsync_callbacks_[slot] = runtime.state().gpr[5];
    }
    runtime.setRegister(2, previous);
    runtime.completeHostCall();
    return {RetailHleStatus::handled, vsync_callbacks_address};
}

bool RetailHle::onVBlank(psx::R3000Runtime& runtime) {
    std::uint32_t counter{};
    if (!runtime.read32(vcount_address, counter) ||
        !runtime.write32(vcount_address, counter + 1U)) {
        return false;
    }
    // PadInitDirect points the retail controller code at these two
    // revision-owned buffers. Refresh port one at the same frame boundary as
    // the original pad driver; byte zero is zero for a connected controller,
    // byte one is the standard digital-pad ID, and bytes two/three are the
    // active-low button mask in controller wire order: low byte first, then
    // high byte. ReadSonyPads combines them high-first into the byte-swapped
    // Psy-Q button convention used by retail gameplay.
    if (!runtime.write8(
            pad_one_buffer_address, pad_one_connected_ ? 0U : 0xFFU) ||
        !runtime.write8(pad_one_buffer_address + 1U, 0x41U) ||
        !runtime.write8(
            pad_one_buffer_address + 2U,
            static_cast<std::uint8_t>(pad_one_buttons_)) ||
        !runtime.write8(
            pad_one_buffer_address + 3U,
            static_cast<std::uint8_t>(pad_one_buttons_ >> 8U)) ||
        !runtime.write8(pad_two_buffer_address, 0xFFU)) {
        return false;
    }
    // Publish the DualShock capability state the retail pad driver would have
    // reached through its SIO detection state machine. These fields are the
    // inputs to PadGetState and PadInfoMode, so the game enables its Options
    // vibration toggle and permits its shake code. Written every VBlank so a
    // PadInitDirect struct reset is re-applied before any retail pad check.
    // Port two keeps its BSS-disconnected state.
    if (pad_one_connected_ &&
        (!runtime.write8(
             pad_struct_port_zero_address + pad_struct_state_offset,
             pad_struct_ready_state) ||
         !runtime.write8(
             pad_struct_port_zero_address + pad_struct_mode_offset,
             pad_struct_ready_mode) ||
         !runtime.write8(
             pad_struct_port_zero_address + pad_struct_comb_count_offset,
             pad_struct_comb_count) ||
         !runtime.write8(
             pad_struct_port_zero_address + pad_struct_actuator_count_offset,
             pad_struct_actuator_count) ||
         !runtime.write16(
             pad_struct_port_zero_address + pad_struct_mode_switch_mask_offset,
             pad_struct_mode_switch_mask) ||
         !runtime.write8(
             pad_struct_port_zero_address + pad_struct_current_mode_offset,
             pad_struct_current_mode) ||
         !runtime.write8(
             pad_struct_port_zero_address + pad_struct_comb_entry_count_offset,
             pad_struct_comb_entry_count) ||
         !runtime.write8(
             pad_struct_port_zero_address +
                 pad_struct_actuators_per_comb_offset,
             pad_struct_actuators_per_comb))) {
        return false;
    }
    ++vblank_count_;
    // Drain the drive at its transfer rate, so an outstanding read finishes a
    // realistic number of VBlanks after it was issued. The rate is per second,
    // so the share owed to this VBlank carries its remainder rather than
    // rounding: the drive keeps one speed in wall-clock terms however often the
    // emulated display refreshes.
    cd_drain_remainder_ += cd_units_per_second_per_speed * cd_read_speed_;
    const auto drained = cd_drain_remainder_ / vblank_rate_;
    cd_drain_remainder_ %= vblank_rate_;
    cd_pending_units_ -= std::min(cd_pending_units_, drained);
    return true;
}

RetailHleResult RetailHle::dispatchMoviePlay(
    psx::R3000Runtime& runtime) {
    // Gate this overlay address by the exact Game::PlayMovie caller because
    // overlay 3 reuses the same address range for unrelated gameplay code.
    // The sink may block the guest worker while the main thread owns native
    // video presentation. With no sink (headless probes and core-only builds),
    // retain the milestone's safe skip fallback.
    if (movie_play_sink_ && !pending_movie_path_.empty()) {
        movie_play_sink_({
            pending_movie_path_, pending_movie_followed_by_movie_});
    }
    pending_movie_path_.clear();
    pending_movie_followed_by_movie_ = false;
    runtime.setRegister(2, 0U);
    runtime.completeHostCall();
    return {RetailHleStatus::handled, movie_play_address};
}

RetailHleResult RetailHle::captureMoviePath(
    psx::R3000Runtime& runtime) {
    // Game::PlayMovie(Game*, const char*, int, int) receives the bare STR
    // filename in a1 before it constructs the overlay-owned MoviePlayer.
    // Capture it here and let retail perform its normal display/audio setup;
    // playback is handed to the host only at MoviePlayer::Play below.
    constexpr std::size_t maximum_path_length = 255U;
    pending_movie_path_.clear();
    pending_movie_followed_by_movie_ = retailMovieCallerChainsAnother(
        runtime.state().gpr[31]);
    const auto address = runtime.state().gpr[5];
    if (address == 0U) {
        pending_movie_followed_by_movie_ = false;
        return {};
    }
    pending_movie_path_.reserve(32U);
    for (std::size_t index = 0U; index < maximum_path_length; ++index) {
        std::uint8_t character{};
        if (!runtime.read8(
                address + static_cast<std::uint32_t>(index), character)) {
            pending_movie_path_.clear();
            pending_movie_followed_by_movie_ = false;
            return {RetailHleStatus::memory_fault, game_play_movie_address};
        }
        if (character == 0U) {
            if (movie_prepare_sink_) {
                movie_prepare_sink_({
                    pending_movie_path_, pending_movie_followed_by_movie_});
            }
            return {};
        }
        pending_movie_path_.push_back(static_cast<char>(character));
    }
    pending_movie_path_.clear();
    pending_movie_followed_by_movie_ = false;
    return {RetailHleStatus::memory_fault, game_play_movie_address};
}

RetailHleResult RetailHle::dispatchCdInit(psx::R3000Runtime& runtime) {
    // The image has already been opened and revision-validated by the host.
    // This is Sony's internal lowercase CD_init, whose ABI returns zero on
    // success (unlike the public PsyCross CdInit wrapper).
    runtime.setRegister(2, 0U);
    cd_status_ = 2U;
    runtime.completeHostCall();
    return {RetailHleStatus::handled, cd_init_address};
}

RetailHleResult RetailHle::dispatchCdControl(psx::R3000Runtime& runtime) {
    constexpr std::uint32_t command_set_location = 0x02U;
    constexpr std::uint32_t command_seek_logical = 0x15U;
    constexpr std::uint8_t status_motor_on = 1U << 1U;
    const auto command = runtime.state().gpr[4] & 0xFFU;
    const auto parameter_address = runtime.state().gpr[5];
    if (command == command_set_location || command == command_seek_logical) {
        std::uint8_t minute_bcd{};
        std::uint8_t second_bcd{};
        std::uint8_t sector_bcd{};
        std::uint32_t minute{};
        std::uint32_t second{};
        std::uint32_t sector{};
        if (parameter_address == 0U ||
            !runtime.read8(parameter_address, minute_bcd) ||
            !runtime.read8(parameter_address + 1U, second_bcd) ||
            !runtime.read8(parameter_address + 2U, sector_bcd)) {
            return {RetailHleStatus::memory_fault, cd_control_address};
        }
        if (!decodeBcd(minute_bcd, 100U, minute) ||
            !decodeBcd(second_bcd, 60U, second) ||
            !decodeBcd(sector_bcd, 75U, sector)) {
            runtime.setRegister(2, std::numeric_limits<std::uint32_t>::max());
            runtime.completeHostCall();
            return {RetailHleStatus::handled, cd_control_address};
        }
        const auto absolute_sector =
            minute * 60U * 75U + second * 75U + sector;
        constexpr std::uint32_t lead_in_sectors = 2U * 75U;
        if (absolute_sector < lead_in_sectors) {
            runtime.setRegister(2, std::numeric_limits<std::uint32_t>::max());
            runtime.completeHostCall();
            return {RetailHleStatus::handled, cd_control_address};
        }
        current_cd_lba_ = absolute_sector - lead_in_sectors;
    }

    const auto result_address = runtime.state().gpr[6];
    if (result_address != 0U) {
        if (!runtime.write8(result_address, status_motor_on)) {
            return {RetailHleStatus::memory_fault, cd_control_address};
        }
        for (std::uint32_t index = 1U; index < 8U; ++index) {
            if (!runtime.write8(result_address + index, 0U)) {
                return {RetailHleStatus::memory_fault, cd_control_address};
            }
        }
    }
    runtime.setRegister(2, 0U);
    cd_status_ = 2U;
    runtime.completeHostCall();
    return {RetailHleStatus::handled, cd_control_address};
}

RetailHleResult RetailHle::dispatchCdRead(psx::R3000Runtime& runtime) {
    const auto sector_count = runtime.state().gpr[4];
    const auto destination = runtime.state().gpr[5];
    if (image_ == nullptr || sector_count == 0U || destination == 0U) {
        runtime.setRegister(2, 0U);
        runtime.completeHostCall();
        return {RetailHleStatus::handled, cd_read_address};
    }

    if (cd_read_sink_) {
        cd_read_sink_(
            current_cd_lba_,
            sector_count,
            destination,
            runtime.state().gpr[31]);
    }
    auto bytes = image_->readDataSectors(current_cd_lba_, sector_count);
    applyCdFileOverrides(current_cd_lba_, bytes);
    if (!runtime.loadBytes(destination, bytes)) {
        return {RetailHleStatus::memory_fault, cd_read_address};
    }
    current_cd_lba_ += sector_count;
    ++cd_read_calls_;
    cd_sectors_read_ += sector_count;
    if (cd_read_speed_ != 0U) {
        // A fresh read supersedes whatever was outstanding rather than queueing
        // behind it. Accumulating instead is what made the first attempt at
        // this run away: retail re-enters its loader while a read is in
        // progress and issues the next chunk, so an additive backlog never
        // drains and boot never finishes.
        cd_pending_units_ =
            static_cast<std::uint64_t>(sector_count) * cd_units_per_sector;
    }
    // CdStatus exposes the drive-status byte here, not the CD interrupt code.
    // The synchronous transfer has completed, so the motor is on and the
    // drive is no longer in the reading state.
    cd_status_ = 2U;
    runtime.setRegister(2, 1U);
    runtime.completeHostCall();
    return {RetailHleStatus::handled, cd_read_address};
}

void RetailHle::addCdFileOverride(
    const std::string& path,
    std::vector<std::byte> bytes) {
    if (image_ == nullptr) {
        throw core::Error{"host menu asset overlay requires a disc image"};
    }
    const auto entry = image_->find(path);
    if (entry.is_directory || bytes.size() != entry.size) {
        throw core::Error{"host menu asset overlay size mismatch: " + path};
    }
    cd_file_overrides_.push_back({entry.extent_lba, std::move(bytes)});
}

void RetailHle::applyCdFileOverrides(
    std::uint32_t first_lba,
    std::span<std::byte> sectors) const {
    const auto read_begin = static_cast<std::uint64_t>(first_lba) *
        iso_sector_size;
    const auto read_end = read_begin + sectors.size();
    for (const auto& override_file : cd_file_overrides_) {
        const auto file_begin =
            static_cast<std::uint64_t>(override_file.extent_lba) *
            iso_sector_size;
        const auto file_end = file_begin + override_file.bytes.size();
        const auto overlap_begin = std::max(read_begin, file_begin);
        const auto overlap_end = std::min(read_end, file_end);
        if (overlap_begin >= overlap_end) {
            continue;
        }
        const auto destination_offset = static_cast<std::size_t>(
            overlap_begin - read_begin);
        const auto source_offset = static_cast<std::size_t>(
            overlap_begin - file_begin);
        const auto count = static_cast<std::size_t>(
            overlap_end - overlap_begin);
        std::copy_n(
            override_file.bytes.begin() +
                static_cast<std::ptrdiff_t>(source_offset),
            count,
            sectors.begin() + static_cast<std::ptrdiff_t>(destination_offset));
    }
}

void RetailHle::enableExperimentalHostMenu() {
    if (image_ == nullptr) {
        throw core::Error{"experimental host menu requires a disc image"};
    }
    static constexpr std::string_view menu_definition =
        "MENU Title Menu_Title\n"
        "BUTTON ResumeGame\nGOTO NewGame ConfirmationNewGame\n"
        "BUTTON LoadGame\nBUTTON SaveGame\nGOTO GameOption Option\n"
        "END MENU\n"
        "MENU Option Menu_GameOption\nGOTO CONTROLLER Controller\n"
        "GOTO SOUND Sound\nGOTO CREDITS HostDSP\nEND MENU\n"
        "MENU HostDSP Menu_Sound\n"
        "SNDSELECT Sound_Effect 1 1\n"
        "SNDSELECT Sound_Music 1 1\n"
        "SNDSELECT Sound_Voice 3 3\nEND MENU\n"
        "MENU Sound Menu_Sound\nSNDSELECT Sound_Effect 6 65535\n"
        "SNDSELECT Sound_Music 6 65535\n"
        "SNDSELECT Sound_Voice 6 65535\nSELECT Sound_Stereo\nEND MENU\n"
        "MENU Controller Menu_Controller\nSHKSELECT Controller_Shock\n"
        "CTLSELECT Controller_Config Menu_Controller\nEND MENU\n"
        "MENU ConfirmationNewGame Menu_Confirmation\nBUTTON No\n"
        "BUTTON Yes\nEND MENU\n"
        "MEMCARD MemCard YesNoOverlay Menu_MemCard\nBUTTON No\n"
        "BUTTON Yes\nEND MENU\n"
        "MENU Level Menu_Location\nBUTTON GoButton\nEND MENU\nENDTEXT\n";
    const auto menu_entry = image_->find("XC/FE_MNU.TXT");
    addCdFileOverride(
        "XC/FE_MNU.TXT",
        asBytes(menu_definition, menu_entry.size));

    auto front_end = image_->readFile("XC/FE.1");
    static constexpr std::array credits{
        std::byte{'C'}, std::byte{'r'}, std::byte{'e'}, std::byte{'d'},
        std::byte{'i'}, std::byte{'t'}, std::byte{'s'}, std::byte{0}};
    static constexpr std::array display{
        std::byte{'D'}, std::byte{'i'}, std::byte{'s'}, std::byte{'p'},
        std::byte{'l'}, std::byte{'a'}, std::byte{'y'}, std::byte{0}};
    const auto label = std::search(
        front_end.begin(), front_end.end(), credits.begin(), credits.end());
    if (label == front_end.end() ||
        std::search(label + 1, front_end.end(), credits.begin(), credits.end()) !=
            front_end.end()) {
        throw core::Error{
            "experimental host menu could not uniquely find Credits label"};
    }
    std::copy(display.begin(), display.end(), label);
    addCdFileOverride("XC/FE.1", std::move(front_end));
    host_menu_enabled_ = true;
}

void RetailHle::setHostMenuState(
    std::uint32_t guest_update_rate,
    HostRenderSize render_size,
    bool widescreen_cull) noexcept {
    host_menu_update_rate_ = guest_update_rate >= 60U ? 60U : 30U;
    const auto narrow = std::ranges::find(
        host_narrow_render_sizes_, render_size);
    const auto wide = std::ranges::find(host_wide_render_sizes_, render_size);
    if (narrow != host_narrow_render_sizes_.end()) {
        host_menu_wide_render_sizes_ = false;
        host_menu_render_size_index_ = static_cast<std::uint32_t>(
            narrow - host_narrow_render_sizes_.begin());
    } else if (wide != host_wide_render_sizes_.end()) {
        host_menu_wide_render_sizes_ = true;
        host_menu_render_size_index_ = static_cast<std::uint32_t>(
            wide - host_wide_render_sizes_.begin());
    } else {
        host_menu_wide_render_sizes_ =
            static_cast<std::uint64_t>(render_size.width) * 3U !=
            static_cast<std::uint64_t>(render_size.height) * 4U;
        host_menu_render_size_index_ = 1U;
    }
    host_menu_widescreen_cull_ = widescreen_cull;
}

bool RetailHle::ensureLicensesMenuItem(
    psx::R3000Runtime& runtime, std::uint32_t manager) {
    if (!host_menu_enabled_ || manager == 0U) {
        return false;
    }
    const auto rd = [&](std::uint32_t address) {
        std::uint32_t value{};
        return runtime.read32(address, value) ? value : 0U;
    };
    // Locate the pause MAIN MENU (Menu_Title) in the manager's menu list.
    std::uint32_t title = 0U;
    for (std::uint32_t candidate = rd(manager + 0x30U), guard = 0U;
         candidate != 0U && guard < 32U;
         candidate = rd(candidate), ++guard) {
        if (rd(candidate + 0x0CU) == menu_title_hash) {
            title = candidate;
            break;
        }
    }
    if (title == 0U) {
        return false;
    }
    // Idempotent: skip if this menu instance already carries the row.
    for (std::uint32_t item = rd(title + 0x18U), guard = 0U;
         item != 0U && guard < 32U;
         item = rd(item), ++guard) {
        if (rd(item + 0x18U) == licenses_item_hash) {
            return false;
        }
    }
    const auto section = rd(manager + 0x20U);
    const auto overlay_inv = section != 0U ? rd(section + 0x14U) : 0U;
    const auto screen_inv = section != 0U ? rd(section + 0x0CU) : 0U;
    if (overlay_inv == 0U || screen_inv == 0U) {
        return false;
    }
    const auto first_item = rd(title + 0x18U);
    const auto title_text = first_item != 0U ? rd(first_item + 0x0CU) : 0U;
    if (title_text == 0U) {
        return false;
    }
    // The overlay that draws the title prims is the one whose prim array holds
    // the first item's text object.
    std::uint32_t target_overlay = 0U;
    std::uint32_t overlay_inv_value_addr = 0U;
    const auto overlay_count = rd(overlay_inv + 0x08U);
    for (std::uint32_t i = 0U; i < overlay_count && i < 64U; ++i) {
        const auto value_addr = overlay_inv + 0x10U + i * 8U;
        const auto overlay = rd(value_addr);
        if (overlay == 0U) {
            continue;
        }
        const auto count = rd(overlay + 4U);
        for (std::uint32_t p = 0U; p < count && p < 64U; ++p) {
            if (rd(overlay + 0x0CU + p * 8U) == title_text) {
                target_overlay = overlay;
                overlay_inv_value_addr = value_addr;
                break;
            }
        }
        if (target_overlay != 0U) {
            break;
        }
    }
    if (target_overlay == 0U) {
        return false;
    }
    const auto nprims = rd(target_overlay + 4U);
    if (nprims == 0U || nprims > 32U) {
        return false;
    }
    // Clone the last existing item's text object: an unselected label with a
    // valid font/style and base colour.
    const auto tail_item = rd(title + 0x1CU);
    const auto clone_source =
        tail_item != 0U ? rd(tail_item + 0x0CU) : title_text;
    if (clone_source == 0U) {
        return false;
    }

    // --- reads validated; construct arena objects, then splice references ---
    // 1. Clone the text object (0x40 bytes covers the ~0x3C struct + frame[1]).
    for (std::uint32_t offset = 0U; offset < 0x40U; offset += 4U) {
        if (!runtime.write32(
                menu_arena_textobj + offset, rd(clone_source + offset))) {
            return false;
        }
    }
    // 2. Repoint the clone's active-frame string at the host "LICENSES" buffer.
    std::uint8_t frame{};
    if (!runtime.read8(menu_arena_textobj + 0x2DU, frame) ||
        !writeText(runtime, menu_arena_string, "Licenses") ||
        !runtime.write32(
            menu_arena_textobj + 0x38U +
                static_cast<std::uint32_t>(frame) * 4U,
            menu_arena_string)) {
        return false;
    }
    // 3. Capture the current item block's vertical extent. The Y translate at
    //    text object +0x18 is screenY<<16. The rows are re-spaced in step 9 so
    //    the extra row fits inside the fixed-size menu frame instead of below
    //    it; here we only record the top (min) and bottom (max) rows.
    std::uint32_t min_y = 0xFFFFFFFFU;
    std::uint32_t max_y = 0U;
    std::uint32_t item_rows = 0U;
    for (std::uint32_t item = rd(title + 0x18U), guard = 0U;
         item != 0U && guard < 16U;
         item = rd(item), ++guard) {
        const auto obj = rd(item + 0x0CU);
        if (obj != 0U) {
            const auto y = rd(obj + 0x18U);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
            ++item_rows;
        }
    }
    // 4. Relocate the overlay: copy header + existing entries, append our prim,
    //    bump the count. The packed section leaves no room to grow in place.
    for (std::uint32_t offset = 0U; offset < 8U + nprims * 8U; offset += 4U) {
        if (!runtime.write32(
                menu_arena_overlay + offset, rd(target_overlay + offset))) {
            return false;
        }
    }
    if (!runtime.write32(menu_arena_overlay + 8U + nprims * 8U,
                         licenses_item_hash) ||
        !runtime.write32(menu_arena_overlay + 0x0CU + nprims * 8U,
                         menu_arena_textobj) ||
        !runtime.write32(menu_arena_overlay + 4U, nprims + 1U)) {
        return false;
    }
    // 5. Build the hdItemButton (next/prev linked during the splice below).
    if (!runtime.write32(menu_arena_button + 0x00U, 0U) ||
        !runtime.write32(menu_arena_button + 0x04U, 0U) ||
        !runtime.write32(menu_arena_button + 0x08U, hd_item_button_vtable) ||
        !runtime.write32(menu_arena_button + 0x0CU, menu_arena_textobj) ||
        !runtime.write32(
            menu_arena_button + 0x10U, host_menu_callback_address) ||
        !runtime.write32(menu_arena_button + 0x14U, 0U) ||
        !runtime.write32(menu_arena_button + 0x18U, licenses_item_hash)) {
        return false;
    }
    // 6. Repoint the section overlay inventory at the relocated overlay.
    if (!runtime.write32(overlay_inv_value_addr, menu_arena_overlay)) {
        return false;
    }
    // 7. Repoint every owning xcScreen entry so screen switches still toggle
    //    the relocated overlay's visibility.
    const auto screen_count = rd(screen_inv + 0x08U);
    for (std::uint32_t i = 0U; i < screen_count && i < 64U; ++i) {
        const auto screen = rd(screen_inv + 0x10U + i * 8U);
        if (screen == 0U) {
            continue;
        }
        const auto novl = rd(screen);
        for (std::uint32_t o = 0U; o < novl && o < 64U; ++o) {
            const auto slot = screen + 4U + o * 4U;
            if (rd(slot) == target_overlay &&
                !runtime.write32(slot, menu_arena_overlay)) {
                return false;
            }
        }
    }
    // 8. Append the button to Menu_Title's item list (ccMinList: head@+0x18,
    //    tail@+0x1C; nodes link next@+0/prev@+4).
    const auto old_tail = rd(title + 0x1CU);
    if (old_tail != 0U) {
        if (!runtime.write32(old_tail + 0x00U, menu_arena_button) ||
            !runtime.write32(menu_arena_button + 0x04U, old_tail)) {
            return false;
        }
    } else if (!runtime.write32(title + 0x18U, menu_arena_button)) {
        return false;
    }
    if (!runtime.write32(title + 0x1CU, menu_arena_button)) {
        return false;
    }
    // 9. Re-space every row evenly so the extra row fits inside the fixed-size
    //    menu frame. The block spans the original top..bottom extent plus a
    //    little extra spread, shifted down slightly so it sits centered rather
    //    than top-heavy.
    const auto total_rows = item_rows + 1U;
    if (min_y <= max_y && total_rows >= 2U) {
        const auto span = (max_y - min_y) + menu_rows_extra_span;
        std::uint32_t index = 0U;
        for (std::uint32_t item = rd(title + 0x18U), guard = 0U;
             item != 0U && guard < 16U;
             item = rd(item), ++guard) {
            const auto obj = rd(item + 0x0CU);
            if (obj != 0U) {
                const auto y = min_y + menu_rows_down_bias +
                    static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(span) * index /
                        (total_rows - 1U));
                if (!runtime.write32(obj + 0x18U, y)) {
                    return false;
                }
                ++index;
            }
        }
    }
    return true;
}

RetailHleResult RetailHle::injectLicensesOnSetTopMenu(
    psx::R3000Runtime& runtime) {
    if (!host_menu_enabled_) {
        return {};
    }
    static constexpr std::array fingerprint{
        0x27BDFFE0U, 0xAFB00010U, 0x00808021U, 0xAFB10014U};
    for (std::size_t index = 0U; index < fingerprint.size(); ++index) {
        std::uint32_t word{};
        if (!runtime.read32(
                set_top_menu_address +
                    static_cast<std::uint32_t>(index * 4U),
                word) ||
            word != fingerprint[index]) {
            return {};
        }
    }
    // Pre-hook: retail still runs SetTopMenu. $a0 is the manager.
    static_cast<void>(ensureLicensesMenuItem(runtime, runtime.state().gpr[4]));
    return {};
}

RetailHleResult RetailHle::inspectHostMenuPush(
    psx::R3000Runtime& runtime) {
    if (!host_menu_enabled_) {
        return {};
    }
    static constexpr std::array fingerprint{
        0x27BDFFE0U, 0xAFB20018U, 0x00809021U, 0xAFB10014U};
    for (std::size_t index = 0U; index < fingerprint.size(); ++index) {
        std::uint32_t word{};
        if (!runtime.read32(
                host_menu_push_address +
                    static_cast<std::uint32_t>(index * 4U),
                word) ||
            word != fingerprint[index]) {
            return {};
        }
    }
    const auto menu = runtime.state().gpr[5];
    std::uint32_t menu_hash{};
    if (menu == 0U || !runtime.read32(menu + 0x0CU, menu_hash)) {
        return {};
    }

    // Splice the native "LICENSES" row into the pause MAIN MENU. Idempotent and
    // independent of which submenu triggered this push; it only acts once
    // Menu_Title exists in the manager.
    static_cast<void>(ensureLicensesMenuItem(runtime, runtime.state().gpr[4]));

    const auto rewriteItem = [&](std::uint32_t item,
                                 std::string_view label,
                                 std::string_view value,
                                 std::uint16_t index) {
        std::uint32_t label_object{};
        std::uint32_t value_object{};
        std::uint8_t frame{};
        std::uint8_t value_frame{};
        std::uint32_t label_text{};
        if (!runtime.write32(item + 0x10U, host_menu_callback_address) ||
            !runtime.write16(item + 0x30U, index) ||
            !runtime.write32(item + 0x34U, index) ||
            !writeText(runtime, item + 0x38U, value) ||
            !runtime.read32(item + 0x0CU, label_object) ||
            !runtime.read32(item + 0x20U, value_object) ||
            !runtime.read8(label_object + 0x2DU, frame) ||
            !runtime.read8(value_object + 0x2DU, value_frame) ||
            !runtime.read32(
                label_object + 0x38U +
                    static_cast<std::uint32_t>(frame) * 4U,
                label_text) ||
            !runtime.write32(
                value_object + 0x38U +
                    static_cast<std::uint32_t>(value_frame) * 4U,
                item + 0x38U)) {
            return false;
        }
        return writeText(runtime, label_text, label);
    };
    const auto restoreItemLabel = [&](std::uint32_t item,
                                      std::string_view label) {
        std::uint32_t label_object{};
        std::uint32_t value_object{};
        std::uint8_t frame{};
        std::uint8_t value_frame{};
        std::uint32_t label_text{};
        return runtime.read32(item + 0x0CU, label_object) &&
            runtime.read32(item + 0x20U, value_object) &&
            runtime.read8(label_object + 0x2DU, frame) &&
            runtime.read8(value_object + 0x2DU, value_frame) &&
            runtime.read32(
                label_object + 0x38U +
                    static_cast<std::uint32_t>(frame) * 4U,
                label_text) &&
            runtime.write32(
                value_object + 0x38U +
                    static_cast<std::uint32_t>(value_frame) * 4U,
                item + 0x38U) &&
            writeText(runtime, label_text, label);
    };
    const auto writeActiveObjectText = [&](std::uint32_t item,
                                           std::uint32_t object_offset,
                                           std::string_view text) {
        std::uint32_t object{};
        std::uint8_t frame{};
        std::uint32_t text_address{};
        return runtime.read32(item + object_offset, object) &&
            runtime.read8(object + 0x2DU, frame) &&
            runtime.read32(
                object + 0x38U + static_cast<std::uint32_t>(frame) * 4U,
                text_address) &&
            writeText(runtime, text_address, text);
    };
    const auto writeSoundScreenTitle = [&](std::uint32_t stereo_item,
                                           std::string_view text) {
        std::uint32_t label_object{};
        std::uint8_t frame{};
        std::uint32_t stereo_text{};
        // In the supported FE.1, "SOUND OPTIONS" immediately follows the
        // null-terminated "Stereo" label. This avoids inventing another
        // overlay object or relying on a process-specific load address.
        return runtime.read32(stereo_item + 0x0CU, label_object) &&
            runtime.read8(label_object + 0x2DU, frame) &&
            runtime.read32(
                label_object + 0x38U +
                    static_cast<std::uint32_t>(frame) * 4U,
                stereo_text) &&
            writeText(runtime, stereo_text + 7U, text);
    };
    static constexpr std::array value_text_style_offsets{
        0x04U, 0x08U, 0x10U, 0x14U, 0x1CU, 0x20U, 0x24U, 0x30U};
    const auto readValueTextStyle = [&](std::uint32_t item,
                                        auto& values) {
        std::uint32_t object{};
        if (!runtime.read32(item + 0x20U, object)) {
            return false;
        }
        for (std::size_t index = 0U;
             index < value_text_style_offsets.size(); ++index) {
            if (!runtime.read32(
                    object + value_text_style_offsets[index],
                    values[index])) {
                return false;
            }
        }
        return true;
    };
    const auto writeValueTextStyle = [&](std::uint32_t item,
                                         const auto& values) {
        std::uint32_t object{};
        if (!runtime.read32(item + 0x20U, object)) {
            return false;
        }
        for (std::size_t index = 0U;
             index < value_text_style_offsets.size(); ++index) {
            if (!runtime.write32(
                    object + value_text_style_offsets[index],
                    values[index])) {
                return false;
            }
        }
        return true;
    };
    const auto copyValueTextStyle = [&](std::uint32_t source_item,
                                        std::uint32_t target_item) {
        std::uint32_t source_object{};
        std::uint32_t target_object{};
        if (!runtime.read32(source_item + 0x20U, source_object) ||
            !runtime.read32(target_item + 0x20U, target_object)) {
            return false;
        }
        // xcTextObj embeds a row-major 3x3 transform at +4. Keep the target
        // row's X/Y translation (matrix elements 2 and 5), but borrow the
        // source's scale/rotation and font. Stereo is the compact source;
        // Music restores the stock volume-readout style.
        for (const auto offset : value_text_style_offsets) {
            std::uint32_t value{};
            if (!runtime.read32(source_object + offset, value) ||
                !runtime.write32(target_object + offset, value)) {
                return false;
            }
        }
        return true;
    };
    const auto findItem = [&](std::uint32_t owner,
                              std::uint32_t wanted_hash) {
        std::uint32_t candidate{};
        if (!runtime.read32(owner + 0x18U, candidate)) {
            return std::uint32_t{};
        }
        for (std::size_t visited = 0U;
             candidate != 0U && visited < 16U;
             ++visited) {
            std::uint32_t candidate_hash{};
            std::uint32_t next{};
            if (!runtime.read32(candidate + 0x18U, candidate_hash) ||
                !runtime.read32(candidate, next)) {
                return std::uint32_t{};
            }
            if (candidate_hash == wanted_hash) {
                return candidate;
            }
            candidate = next;
        }
        return std::uint32_t{};
    };
    const auto findMenu = [&](std::uint32_t manager,
                              std::uint32_t wanted_hash) {
        std::uint32_t candidate{};
        if (manager == 0U || !runtime.read32(manager + 0x30U, candidate)) {
            return std::uint32_t{};
        }
        for (std::size_t visited = 0U;
             candidate != 0U && visited < 16U;
             ++visited) {
            std::uint32_t candidate_hash{};
            std::uint32_t next{};
            if (!runtime.read32(candidate + 0x0CU, candidate_hash) ||
                !runtime.read32(candidate, next)) {
                return std::uint32_t{};
            }
            if (candidate_hash == wanted_hash) {
                return candidate;
            }
            candidate = next;
        }
        return std::uint32_t{};
    };

    if (menu_hash != host_display_menu_hash && menu_hash != sound_menu_hash) {
        return {};
    }
    std::uint32_t item{};
    if (!runtime.read32(menu + 0x18U, item)) {
        return {};
    }
    std::size_t visited = 0U;
    while (item != 0U && visited++ < 16U) {
        std::uint32_t item_hash{};
        std::uint32_t next{};
        if (!runtime.read32(item + 0x18U, item_hash) ||
            !runtime.read32(item, next)) {
            return {RetailHleStatus::memory_fault, item};
        }
        if (item_hash == frame_rate_item_hash) {
            if (menu_hash == host_display_menu_hash) {
                const auto sixty = host_menu_update_rate_ >= 60U;
                if (!rewriteItem(
                        item,
                        "Frame Rate",
                        sixty ? "60 HZ" : "30 HZ",
                        sixty ? 1U : 0U)) {
                    return {RetailHleStatus::memory_fault, item};
                }
            } else if (!restoreItemLabel(item, "Sound Effects")) {
                return {RetailHleStatus::memory_fault, item};
            }
        } else if (item_hash == resolution_item_hash) {
            if (menu_hash == host_display_menu_hash) {
                const auto& sizes = activeHostRenderSizes();
                const auto& size = sizes[
                    std::min<std::size_t>(
                        host_menu_render_size_index_,
                        sizes.size() - 1U)];
                const auto text = std::to_string(size.width) + "x" +
                    std::to_string(size.height);
                if (!rewriteItem(
                        item,
                        "Resolution",
                        text,
                        static_cast<std::uint16_t>(
                            host_menu_render_size_index_))) {
                    return {RetailHleStatus::memory_fault, item};
                }
            } else if (!restoreItemLabel(item, "Voice Over")) {
                return {RetailHleStatus::memory_fault, item};
            }
        } else if (item_hash == sound_music_item_hash) {
            if (menu_hash == host_display_menu_hash) {
                if (!rewriteItem(
                        item,
                        "Widescreen",
                        host_menu_widescreen_cull_ ? "ON" : "OFF",
                        host_menu_widescreen_cull_ ? 1U : 0U)) {
                    return {RetailHleStatus::memory_fault, item};
                }
            } else if (!restoreItemLabel(item, "Music")) {
                return {RetailHleStatus::memory_fault, item};
            }
        } else if (menu_hash == sound_menu_hash &&
                   item_hash == sound_stereo_item_hash) {
            std::uint32_t value_object{};
            std::uint32_t off_text{};
            std::uint32_t on_text{};
            if (!writeActiveObjectText(item, 0x0CU, "Stereo") ||
                !writeSoundScreenTitle(item, "SOUND OPTIONS") ||
                !runtime.read32(item + 0x20U, value_object) ||
                !runtime.read32(value_object + 0x38U, off_text) ||
                !runtime.read32(value_object + 0x3CU, on_text) ||
                !writeText(runtime, off_text, "OFF") ||
                !writeText(runtime, on_text, "ON")) {
                return {RetailHleStatus::memory_fault, item};
            }
        }
        item = next;
    }
    if (menu_hash == host_display_menu_hash) {
        // Menu_Sound owns four rows independently of the logical hdMenu item
        // list. Blank the one unused row through the real Sound menu's object
        // references; its next push restores it before InstallMenu runs.
        const auto sound_menu = findMenu(runtime.state().gpr[4U], sound_menu_hash);
        const auto music_item = findItem(sound_menu, sound_music_item_hash);
        const auto stereo_item = findItem(sound_menu, sound_stereo_item_hash);
        const auto frame_item = findItem(menu, frame_rate_item_hash);
        const auto widescreen_item = findItem(menu, sound_music_item_hash);
        const auto resolution_item = findItem(menu, resolution_item_hash);
        if (sound_menu == 0U || music_item == 0U || stereo_item == 0U ||
            frame_item == 0U || widescreen_item == 0U ||
            resolution_item == 0U ||
            (!host_menu_volume_style_valid_ &&
             !readValueTextStyle(
                 widescreen_item, host_menu_volume_style_)) ||
            !copyValueTextStyle(stereo_item, frame_item) ||
            !copyValueTextStyle(stereo_item, widescreen_item) ||
            !copyValueTextStyle(stereo_item, resolution_item) ||
            !writeActiveObjectText(stereo_item, 0x0CU, "") ||
            !writeActiveObjectText(stereo_item, 0x20U, "") ||
            !writeSoundScreenTitle(stereo_item, "DISPLAY MENU")) {
            return {RetailHleStatus::memory_fault, sound_menu};
        }
        host_menu_volume_style_valid_ = true;
    } else {
        const auto music_item = findItem(menu, sound_music_item_hash);
        const auto frame_item = findItem(menu, frame_rate_item_hash);
        const auto voice_item = findItem(menu, resolution_item_hash);
        if (music_item == 0U || frame_item == 0U || voice_item == 0U ||
            (host_menu_volume_style_valid_ &&
             !writeValueTextStyle(
                 music_item, host_menu_volume_style_)) ||
            !copyValueTextStyle(music_item, frame_item) ||
            !copyValueTextStyle(music_item, voice_item)) {
            return {RetailHleStatus::memory_fault, music_item};
        }
        host_menu_volume_style_valid_ = false;
    }
    // This is a pre-hook. Retail still runs PushMenu and owns the transition.
    return {};
}

RetailHleResult RetailHle::dispatchHostMenuCallback(
    psx::R3000Runtime& runtime) {
    if (!host_menu_enabled_) {
        return {};
    }
    const auto item = runtime.state().gpr[4];
    std::uint32_t item_hash{};
    std::uint16_t index{};
    if (item == 0U || !runtime.read32(item + 0x18U, item_hash) ||
        !runtime.read16(item + 0x30U, index)) {
        return {RetailHleStatus::memory_fault, host_menu_callback_address};
    }
    HostMenuEvent event{};
    if (item_hash == frame_rate_item_hash) {
        host_menu_update_rate_ = index == 0U ? 30U : 60U;
        event = {
            HostMenuCommand::guest_update_rate, host_menu_update_rate_, 0U, 0U};
        if (!writeText(
                runtime,
                item + 0x38U,
                host_menu_update_rate_ == 60U ? "60 HZ" : "30 HZ")) {
            return {RetailHleStatus::memory_fault, item};
        }
    } else if (item_hash == resolution_item_hash) {
        const auto& sizes = activeHostRenderSizes();
        host_menu_render_size_index_ = std::min<std::uint32_t>(
            index,
            static_cast<std::uint32_t>(sizes.size() - 1U));
        const auto& size = sizes[host_menu_render_size_index_];
        event = {HostMenuCommand::render_size, 0U, size.width, size.height};
        const auto text = std::to_string(size.width) + "x" +
            std::to_string(size.height);
        if (!writeText(runtime, item + 0x38U, text)) {
            return {RetailHleStatus::memory_fault, item};
        }
    } else if (item_hash == sound_music_item_hash) {
        host_menu_widescreen_cull_ = index != 0U;
        host_menu_wide_render_sizes_ = host_menu_widescreen_cull_;
        const auto& size =
            activeHostRenderSizes()[host_menu_render_size_index_];
        event = {
            HostMenuCommand::widescreen_cull,
            host_menu_widescreen_cull_ ? 1U : 0U,
            size.width,
            size.height};
        if (!writeText(
                runtime,
                item + 0x38U,
                host_menu_widescreen_cull_ ? "ON" : "OFF")) {
            return {RetailHleStatus::memory_fault, item};
        }
        // HostDSP's rows are ordered Frame Rate, Widescreen, Resolution.
        // Retarget the following selector immediately so the open menu shows
        // the newly active aspect family before the next push.
        std::uint32_t resolution_item{};
        if (!runtime.read32(item, resolution_item)) {
            return {RetailHleStatus::memory_fault, item};
        }
        std::uint32_t resolution_hash{};
        if (resolution_item == 0U ||
            !runtime.read32(resolution_item + 0x18U, resolution_hash) ||
            resolution_hash != resolution_item_hash ||
            !runtime.write16(
                resolution_item + 0x30U,
                static_cast<std::uint16_t>(
                    host_menu_render_size_index_))) {
            return {RetailHleStatus::memory_fault, resolution_item};
        }
        const auto text = std::to_string(size.width) + "x" +
            std::to_string(size.height);
        if (!writeText(runtime, resolution_item + 0x38U, text)) {
            return {RetailHleStatus::memory_fault, resolution_item};
        }
    } else if (item_hash == licenses_item_hash) {
        // Injected MAIN MENU row: the guest owns navigation/rendering; selecting
        // it only asks the host to open the license viewer overlay.
        event = {HostMenuCommand::show_licenses, 0U, 0U, 0U};
    } else {
        return {RetailHleStatus::memory_fault, item};
    }
    if (host_menu_sink_) {
        host_menu_sink_(event);
    }
    runtime.setRegister(2U, 0U);
    runtime.completeHostCall();
    return {RetailHleStatus::handled, host_menu_callback_address};
}

RetailHleResult RetailHle::aliasHostMenuScreen(
    psx::R3000Runtime& runtime) {
    if (!host_menu_enabled_) {
        return {};
    }
    // This is the wrapper's call site for MenuMgr::PushMenu. Waiting until
    // here avoids making feMenuMgr's earlier Sound-specific setup mistake the
    // Display menu for the real Sound menu.
    static constexpr std::array fingerprint{
        0x02402021U, 0x0C017E0CU, 0x02202821U};
    for (std::size_t index = 0U; index < fingerprint.size(); ++index) {
        std::uint32_t word{};
        if (!runtime.read32(
                host_menu_screen_alias_address +
                    static_cast<std::uint32_t>(index * 4U),
                word) ||
            word != fingerprint[index]) {
            return {};
        }
    }
    const auto menu = runtime.state().gpr[17U]; // $s1
    std::uint32_t menu_hash{};
    if (menu == 0U || !runtime.read32(menu + 0x0CU, menu_hash) ||
        menu_hash != host_display_menu_hash) {
        return {};
    }
    if (!runtime.write32(menu + 0x0CU, sound_menu_hash)) {
        return {RetailHleStatus::memory_fault, menu + 0x0CU};
    }
    return {};
}

RetailHleResult RetailHle::restoreHostMenuId(
    psx::R3000Runtime& runtime) {
    if (!host_menu_enabled_) {
        return {};
    }
    static constexpr std::array fingerprint{
        0x8FBF001CU, 0x8FB20018U, 0x8FB10014U, 0x8FB00010U};
    for (std::size_t index = 0U; index < fingerprint.size(); ++index) {
        std::uint32_t word{};
        if (!runtime.read32(
                host_menu_id_restore_address +
                    static_cast<std::uint32_t>(index * 4U),
                word) ||
            word != fingerprint[index]) {
            return {};
        }
    }
    const auto menu = runtime.state().gpr[17U]; // $s1 survives the call
    std::uint32_t menu_hash{};
    std::uint32_t item{};
    if (menu == 0U || !runtime.read32(menu + 0x0CU, menu_hash) ||
        menu_hash != sound_menu_hash ||
        !runtime.read32(menu + 0x18U, item)) {
        return {};
    }
    // A real Sound push reaches the same return site. Identify the temporary
    // alias by the callback installed only on our first selector. This also
    // makes the alias recoverable across a quick save taken between the two
    // boundaries, without serializing host-only transient state.
    bool host_display = false;
    for (std::size_t visited = 0U; item != 0U && visited < 16U; ++visited) {
        std::uint32_t item_hash{};
        std::uint32_t callback{};
        std::uint32_t next{};
        if (!runtime.read32(item + 0x18U, item_hash) ||
            !runtime.read32(item + 0x10U, callback) ||
            !runtime.read32(item, next)) {
            return {RetailHleStatus::memory_fault, item};
        }
        if (item_hash == frame_rate_item_hash &&
            callback == host_menu_callback_address) {
            host_display = true;
            break;
        }
        item = next;
    }
    if (!host_display) {
        return {};
    }
    if (!runtime.write32(menu + 0x0CU, host_display_menu_hash)) {
        return {RetailHleStatus::memory_fault, menu + 0x0CU};
    }
    return {};
}

RetailHleResult RetailHle::dispatchCdReadSync(psx::R3000Runtime& runtime) {
    // `CdReadSync(mode, result)` reports the sectors still to come. Mode 0
    // blocks on hardware and so can only ever report completion; mode 1 polls,
    // and retail's loader at 0x8007EF20 uses that one -- a positive remainder
    // makes it return state 5 and come back, which is what gives a screen shown
    // during a load its duration.
    const auto polling = runtime.state().gpr[4] != 0U;
    const auto remaining =
        cd_read_speed_ != 0U && polling ? cdSectorsPending() : 0U;
    if (remaining == 0U) {
        // A blocking wait has by definition finished when it returns.
        cd_pending_units_ = 0U;
        cd_status_ = 0U;
    }
    runtime.setRegister(2, static_cast<std::uint32_t>(remaining));
    runtime.completeHostCall();
    return {RetailHleStatus::handled, cd_read_sync_address};
}

RetailHleResult RetailHle::dispatchCdStatus(psx::R3000Runtime& runtime) {
    runtime.setRegister(2, cd_status_);
    runtime.completeHostCall();
    return {RetailHleStatus::handled, cd_status_address};
}

RetailHleResult RetailHle::dispatchCdReady(psx::R3000Runtime& runtime) {
    constexpr std::uint8_t status_motor_on = 1U << 1U;
    constexpr std::uint32_t complete = 2U;
    cd_status_ = complete;
    const auto result_address = runtime.state().gpr[5];
    if (result_address != 0U &&
        !runtime.write8(result_address, status_motor_on)) {
        return {RetailHleStatus::memory_fault, cd_ready_address};
    }
    runtime.setRegister(2, complete);
    runtime.completeHostCall();
    return {RetailHleStatus::handled, cd_ready_address};
}

RetailHleResult RetailHle::dispatchCdSync(psx::R3000Runtime& runtime) {
    // No asynchronous command is pending in the bootstrap CD shim. Report
    // the Psy-Q "complete" status. A non-null result buffer is left untouched
    // until command/result synthesis is implemented.
    constexpr std::uint32_t complete = 2U;
    runtime.setRegister(2, complete);
    runtime.completeHostCall();
    return {RetailHleStatus::handled, cd_sync_address};
}

RetailHleResult RetailHle::dispatchVSync(psx::R3000Runtime& runtime) {
    std::uint32_t counter{};
    if (!runtime.read32(vcount_address, counter)) {
        return {RetailHleStatus::memory_fault, vsync_address};
    }

    const auto mode = std::bit_cast<std::int32_t>(runtime.state().gpr[4]);
    if (mode < 0) {
        runtime.setRegister(2, counter);
        runtime.completeHostCall();
        return {RetailHleStatus::handled, vsync_address};
    }

    if (!video_timing_baseline_initialized_) {
        video_timing_baseline_ = counter;
        video_timing_baseline_initialized_ = true;
    }
    const auto elapsed = counter - video_timing_baseline_;
    if (mode == 1) {
        runtime.setRegister(2, elapsed);
        runtime.completeHostCall();
        return {RetailHleStatus::handled, vsync_address};
    }

    const auto wait = mode > 1 ? static_cast<std::uint32_t>(mode) : 1U;
    if (elapsed < wait) {
        counter += wait - elapsed;
        if (!runtime.write32(vcount_address, counter)) {
            return {RetailHleStatus::memory_fault, vsync_address};
        }
    }
    video_timing_baseline_ = counter;
    runtime.setRegister(2, elapsed);
    runtime.completeHostCall();
    return {RetailHleStatus::handled, vsync_address};
}

} // namespace stuntmaster::game
