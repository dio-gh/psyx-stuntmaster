#pragma once

#include "stuntmaster/psx/r3000_runtime.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace stuntmaster::core {
class StateReader;
class StateWriter;
}

namespace stuntmaster::psx {

// A PS1 SPU: it consumes the register writes retail already makes and owns the
// 512 KB of sound RAM those registers address.
//
// Retail links its own copy of Sony's `libspu`, so it never calls into
// PsyCross's reimplementation of that library -- it writes hardware registers
// at 0x1F801C00. Driving PsyCross
// instead would mean intercepting twenty-odd retail library entry points by
// address and decoding their struct layouts, which is revision-specific work
// that the register interface makes unnecessary.
//
// The mixer decodes the guest's ADPCM voices and runs the SPU's reverb unit
// against the work area retail reserves in sound RAM. The resulting 44.1 kHz
// stereo stream remains deterministic and independent of any host audio device.
class Spu final : public R3000MmioBus {
public:
    static constexpr std::uint32_t sound_ram_bytes = 512U * 1024U;
    static constexpr std::uint32_t voice_count = 24U;

    // The SPU occupies 0x1F801C00..0x1F801E7F: 24 voices of eight halfword
    // registers, then the control block, then the reverb block.
    static constexpr std::uint32_t register_base = 0x1F801C00U;
    static constexpr std::uint32_t register_end = 0x1F801E80U;

    [[nodiscard]] static constexpr bool owns(
        std::uint32_t physical_address) noexcept {
        return physical_address >= register_base &&
            physical_address < register_end;
    }

    [[nodiscard]] bool readMmio(
        std::uint32_t physical_address,
        R3000AccessWidth width,
        std::uint32_t& value) noexcept override;
    [[nodiscard]] bool writeMmio(
        std::uint32_t physical_address,
        R3000AccessWidth width,
        std::uint32_t value) noexcept override;

    // A DMA4 block transfer into sound RAM, which is how retail uploads its
    // samples. The destination is the transfer address register, which the
    // hardware advances as it writes.
    void writeSoundRam(std::span<const std::uint16_t> words) noexcept;

    struct Voice {
        std::uint16_t volume_left{};
        std::uint16_t volume_right{};
        // The pitch counter step: 0x1000 is 44100 Hz, so this is a 12-bit
        // fixed-point ratio against the SPU's own sample rate.
        std::uint16_t sample_rate{};
        // Both address registers are in eight-byte units, which is the ADPCM
        // block size.
        std::uint16_t start_address{};
        std::uint16_t adsr_lo{};
        std::uint16_t adsr_hi{};
        std::uint16_t current_volume{};
        std::uint16_t repeat_address{};
    };

    [[nodiscard]] const Voice& voice(std::uint32_t index) const noexcept {
        return voices_[index];
    }
    [[nodiscard]] std::span<const std::uint8_t> soundRam() const noexcept {
        return sound_ram_;
    }
    [[nodiscard]] std::uint32_t control() const noexcept { return control_; }
    [[nodiscard]] std::uint16_t mainVolumeLeft() const noexcept {
        return main_volume_left_;
    }
    [[nodiscard]] std::uint16_t mainVolumeRight() const noexcept {
        return main_volume_right_;
    }
    [[nodiscard]] std::uint16_t reverbVolumeLeft() const noexcept {
        return reverb_volume_left_;
    }
    [[nodiscard]] std::uint16_t reverbVolumeRight() const noexcept {
        return reverb_volume_right_;
    }
    [[nodiscard]] std::uint32_t reverbVoices() const noexcept {
        return reverb_ & 0x00FFFFFFU;
    }
    [[nodiscard]] std::uint32_t reverbWorkAddress() const noexcept {
        return static_cast<std::uint32_t>(reverb_work_address_) * 8U;
    }
    [[nodiscard]] std::span<const std::uint16_t> reverbRegisters()
        const noexcept {
        return reverb_registers_;
    }

    // Voices whose key-on edge has not been consumed yet. Reading clears them,
    // because a key-on is an edge and the register does not stay set.
    [[nodiscard]] std::uint32_t takeKeyOn() noexcept {
        const auto keyed = key_on_;
        key_on_ = 0U;
        return keyed;
    }
    [[nodiscard]] std::uint32_t takeKeyOff() noexcept {
        const auto keyed = key_off_;
        key_off_ = 0U;
        return keyed;
    }

    // Decode and mix `frames` stereo sample pairs at 44100 Hz into `out`, which
    // must hold two samples per frame.
    //
    // The guest advances 60 VBlanks a second, so 735 frames a VBlank keeps the
    // mixer on the guest's own clock rather than a host one.
    void mix(std::span<std::int16_t> out) noexcept;

    // Advance voice, ADSR, IRQ, and reverb state without retaining output.
    // Native movies use this while the guest CPU is caller-gated so an active
    // voice ages on wall-clock time instead of resuming mid-sample afterward.
    void advance(std::uint64_t frames) noexcept;

    [[nodiscard]] std::uint32_t activeVoices() const noexcept;
    [[nodiscard]] std::uint32_t activeVoiceMask() const noexcept;

    // Playback reached the address retail asked to be interrupted at. Retail
    // streams its music into a buffer and refills the half it is not playing
    // when this fires, so without it the buffer never refills and the same few
    // seconds loop for ever.
    [[nodiscard]] bool takeIrq() noexcept {
        const auto pending = irq_pending_;
        irq_pending_ = false;
        return pending;
    }
    [[nodiscard]] std::uint64_t irqCount() const noexcept {
        return irq_count_;
    }

    // Counters, so a headless probe can show the guest is really driving this
    // before any of it is audible.
    [[nodiscard]] std::uint64_t registerWrites() const noexcept {
        return register_writes_;
    }
    [[nodiscard]] std::uint64_t keyOnCount() const noexcept {
        return key_on_count_;
    }
    [[nodiscard]] std::uint64_t keyOffCount() const noexcept {
        return key_off_count_;
    }
    [[nodiscard]] std::uint64_t soundRamBytesWritten() const noexcept {
        return sound_ram_bytes_written_;
    }
    // The playback address retail asked to be interrupted at, in bytes. Zero
    // means it never set one.
    [[nodiscard]] std::uint32_t irqAddress() const noexcept {
        return static_cast<std::uint32_t>(irq_address_) * 8U;
    }
    [[nodiscard]] bool irqEnabled() const noexcept {
        return (control_ & 0x0040U) != 0U;
    }
    [[nodiscard]] std::uint32_t transferAddress() const noexcept {
        return transfer_address_;
    }
    void writeState(core::StateWriter& writer) const;
    [[nodiscard]] bool readState(core::StateReader& reader);

private:
    // Where a voice is in sound RAM, which the register file does not describe:
    // the registers say where a sample starts and how fast to walk it, and this
    // is the walk itself.
    // Attack rises to full, decay falls to the sustain level, sustain holds
    // there until key-off, release falls to silence. A voice is only finished
    // when release reaches zero, which is why key-off cannot simply stop it.
    enum class AdsrPhase { off, attack, decay, sustain, release };

    struct VoiceState {
        bool active{};
        AdsrPhase phase{AdsrPhase::off};
        // 0..0x7FFF. The maximum is one short of unity, which is the hardware's
        // own behaviour and not a rounding slip.
        std::int32_t envelope{};
        std::uint32_t envelope_counter{};
        // Byte addresses. The registers hold both in eight-byte units.
        std::uint32_t block_address{};
        std::uint32_t repeat_address{};
        std::uint32_t sample_index{};
        // Twelve-bit fixed point against the SPU's own 44100 Hz rate.
        std::uint32_t pitch_counter{};
        // The ADPCM predictor is second order, so decoding a block needs the
        // two samples before it.
        std::int32_t history1{};
        std::int32_t history2{};
        std::uint8_t flags{};
        bool decoded{};
        std::array<std::int16_t, 28U> samples{};
    };

    [[nodiscard]] std::uint16_t readRegister(std::uint32_t offset) noexcept;
    void writeRegister(std::uint32_t offset, std::uint16_t value) noexcept;
    void decodeBlock(std::uint32_t index) noexcept;
    void tickEnvelope(std::uint32_t index) noexcept;
    [[nodiscard]] std::int32_t nextVoiceSample(std::uint32_t index) noexcept;
    [[nodiscard]] std::uint32_t reverbMemoryAddress(
        std::uint32_t address) const noexcept;
    [[nodiscard]] std::int16_t reverbRead(
        std::uint16_t address, std::int32_t offset = 0) const noexcept;
    void reverbWrite(std::uint16_t address, std::int16_t value) noexcept;
    [[nodiscard]] std::array<std::int32_t, 2U> processReverb(
        std::int32_t left, std::int32_t right) noexcept;

    std::array<VoiceState, voice_count> voice_states_{};

    std::vector<std::uint8_t> sound_ram_ =
        std::vector<std::uint8_t>(sound_ram_bytes, 0U);
    std::array<Voice, voice_count> voices_{};
    std::uint16_t main_volume_left_{};
    std::uint16_t main_volume_right_{};
    std::uint16_t reverb_volume_left_{};
    std::uint16_t reverb_volume_right_{};
    std::uint32_t key_on_{};
    std::uint32_t key_off_{};
    std::uint32_t pitch_modulation_{};
    std::uint32_t noise_{};
    std::uint32_t reverb_{};
    std::uint32_t voice_status_{};
    std::uint16_t reverb_work_address_{};
    std::uint16_t irq_address_{};
    // The transfer address register is in eight-byte units; the running
    // destination is a byte address the hardware advances as data arrives.
    std::uint16_t transfer_address_register_{};
    std::uint32_t transfer_address_{};
    std::uint16_t control_{};
    std::uint16_t transfer_control_{};
    std::uint16_t cd_volume_left_{};
    std::uint16_t cd_volume_right_{};
    std::uint16_t extern_volume_left_{};
    std::uint16_t extern_volume_right_{};
    std::array<std::uint16_t, 32U> reverb_registers_{};
    std::array<std::array<std::int16_t, 128U>, 2U>
        reverb_downsample_buffer_{};
    std::array<std::array<std::int16_t, 64U>, 2U>
        reverb_upsample_buffer_{};
    // Halfword address inside sound RAM. Reverb runs at 22.05 kHz, so this
    // advances once for every two output frames and wraps to mBASE.
    std::uint32_t reverb_current_address_{};
    std::uint8_t reverb_resample_position_{};
    std::uint64_t register_writes_{};
    std::uint64_t key_on_count_{};
    std::uint64_t key_off_count_{};
    std::uint64_t sound_ram_bytes_written_{};
    std::uint64_t irq_count_{};
    bool irq_pending_{};
};

} // namespace stuntmaster::psx
