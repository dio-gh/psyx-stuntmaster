// Reverb processing adapted from SF-pc-port d9522cd under the MIT License.
#include "stuntmaster/psx/spu.hpp"

#include "stuntmaster/core/state_archive.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace stuntmaster::psx {
namespace {

constexpr std::uint32_t voice_block_end = 0x180U; // 24 voices * 16 bytes
constexpr std::uint32_t control_block_end = 0x1C0U;
constexpr std::uint16_t control_reverb_enable = 1U << 7U;
constexpr std::uint32_t reverb_address_mask =
    (Spu::sound_ram_bytes - 1U) / 2U;

// The hardware reverb works at half the SPU sample rate. These are the fixed
// FIR coefficients used to downsample its input and interpolate its output.
constexpr std::array<std::int32_t, 20U> reverb_resample_coefficients{
    -0x0001, 0x0002,  -0x000a, 0x0023,  -0x0067, 0x010a,  -0x0268,
    0x0534,  -0x0b90, 0x2806,  0x2806,  -0x0b90, 0x0534,  -0x0268,
    0x010a,  -0x0067, 0x0023,  -0x000a, 0x0002,  -0x0001,
};

// Both SPU address registers count in eight-byte units, which is the size of an
// ADPCM block.
[[nodiscard]] constexpr std::uint32_t toByteAddress(
    std::uint16_t units) noexcept {
    return static_cast<std::uint32_t>(units) * 8U;
}

[[nodiscard]] std::int16_t clampSample(std::int64_t value) noexcept {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(
        value,
        std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
}

// Arithmetic right shift is implementation-defined for negative signed
// values. Spell out the floor division used by the SPU's fixed-point math.
[[nodiscard]] std::int64_t floorDivPowerOfTwo(
    std::int64_t value, std::uint32_t shift) noexcept {
    const auto divisor = std::int64_t{1} << shift;
    if (value >= 0) {
        return value / divisor;
    }
    return -((-value + divisor - 1) / divisor);
}

} // namespace

void Spu::writeState(core::StateWriter& writer) const {
    writer.pod(voice_states_);
    writer.bytes(std::as_bytes(std::span{sound_ram_}));
    writer.pod(voices_);
    writer.pod(main_volume_left_);
    writer.pod(main_volume_right_);
    writer.pod(reverb_volume_left_);
    writer.pod(reverb_volume_right_);
    writer.pod(key_on_);
    writer.pod(key_off_);
    writer.pod(pitch_modulation_);
    writer.pod(noise_);
    writer.pod(reverb_);
    writer.pod(voice_status_);
    writer.pod(reverb_work_address_);
    writer.pod(irq_address_);
    writer.pod(transfer_address_register_);
    writer.pod(transfer_address_);
    writer.pod(control_);
    writer.pod(transfer_control_);
    writer.pod(cd_volume_left_);
    writer.pod(cd_volume_right_);
    writer.pod(extern_volume_left_);
    writer.pod(extern_volume_right_);
    writer.pod(reverb_registers_);
    writer.pod(reverb_downsample_buffer_);
    writer.pod(reverb_upsample_buffer_);
    writer.pod(reverb_current_address_);
    writer.pod(reverb_resample_position_);
    writer.pod(register_writes_);
    writer.pod(key_on_count_);
    writer.pod(key_off_count_);
    writer.pod(sound_ram_bytes_written_);
    writer.pod(irq_count_);
    writer.pod(irq_pending_);
}

bool Spu::readState(core::StateReader& reader) {
    return reader.pod(voice_states_) &&
        reader.bytes(std::as_writable_bytes(std::span{sound_ram_})) &&
        reader.pod(voices_) && reader.pod(main_volume_left_) &&
        reader.pod(main_volume_right_) &&
        reader.pod(reverb_volume_left_) &&
        reader.pod(reverb_volume_right_) && reader.pod(key_on_) &&
        reader.pod(key_off_) && reader.pod(pitch_modulation_) &&
        reader.pod(noise_) && reader.pod(reverb_) &&
        reader.pod(voice_status_) && reader.pod(reverb_work_address_) &&
        reader.pod(irq_address_) &&
        reader.pod(transfer_address_register_) &&
        reader.pod(transfer_address_) && reader.pod(control_) &&
        reader.pod(transfer_control_) && reader.pod(cd_volume_left_) &&
        reader.pod(cd_volume_right_) && reader.pod(extern_volume_left_) &&
        reader.pod(extern_volume_right_) && reader.pod(reverb_registers_) &&
        reader.pod(reverb_downsample_buffer_) &&
        reader.pod(reverb_upsample_buffer_) &&
        reader.pod(reverb_current_address_) &&
        reader.pod(reverb_resample_position_) &&
        reader.pod(register_writes_) && reader.pod(key_on_count_) &&
        reader.pod(key_off_count_) &&
        reader.pod(sound_ram_bytes_written_) && reader.pod(irq_count_) &&
        reader.pod(irq_pending_);
}

bool Spu::readMmio(
    std::uint32_t physical_address,
    R3000AccessWidth width,
    std::uint32_t& value) noexcept {
    if (!owns(physical_address)) {
        return false;
    }
    const auto offset = physical_address - register_base;
    switch (width) {
    case R3000AccessWidth::halfword:
        value = readRegister(offset);
        return true;
    case R3000AccessWidth::word:
        // A word read spans the halfword pair, low half first.
        value = static_cast<std::uint32_t>(readRegister(offset)) |
            (static_cast<std::uint32_t>(readRegister(offset + 2U)) << 16U);
        return true;
    case R3000AccessWidth::byte: {
        const auto halfword = readRegister(offset & ~1U);
        value = (offset & 1U) != 0U
            ? static_cast<std::uint32_t>(halfword >> 8U)
            : static_cast<std::uint32_t>(halfword & 0xFFU);
        return true;
    }
    }
    return false;
}

bool Spu::writeMmio(
    std::uint32_t physical_address,
    R3000AccessWidth width,
    std::uint32_t value) noexcept {
    if (!owns(physical_address)) {
        return false;
    }
    ++register_writes_;
    const auto offset = physical_address - register_base;
    switch (width) {
    case R3000AccessWidth::halfword:
        writeRegister(offset, static_cast<std::uint16_t>(value));
        return true;
    case R3000AccessWidth::word:
        writeRegister(offset, static_cast<std::uint16_t>(value));
        writeRegister(offset + 2U, static_cast<std::uint16_t>(value >> 16U));
        return true;
    case R3000AccessWidth::byte: {
        // The SPU has no byte ports; merge into the containing halfword rather
        // than dropping the write.
        const auto aligned = offset & ~1U;
        const auto current = readRegister(aligned);
        const auto merged = (offset & 1U) != 0U
            ? static_cast<std::uint16_t>(
                  (current & 0x00FFU) |
                  (static_cast<std::uint16_t>(value & 0xFFU) << 8U))
            : static_cast<std::uint16_t>(
                  (current & 0xFF00U) |
                  static_cast<std::uint16_t>(value & 0xFFU));
        writeRegister(aligned, merged);
        return true;
    }
    }
    return false;
}

std::uint16_t Spu::readRegister(std::uint32_t offset) noexcept {
    if (offset < voice_block_end) {
        const auto& voice = voices_[offset / 16U];
        switch (offset % 16U) {
        case 0x0U: return voice.volume_left;
        case 0x2U: return voice.volume_right;
        case 0x4U: return voice.sample_rate;
        case 0x6U: return voice.start_address;
        case 0x8U: return voice.adsr_lo;
        case 0xAU: return voice.adsr_hi;
        case 0xCU:
            // Retail can poll this to see how far a note has got, so report the
            // live envelope rather than whatever was last written.
            return static_cast<std::uint16_t>(
                voice_states_[offset / 16U].envelope);
        default: return voice.repeat_address;
        }
    }
    if (offset < control_block_end) {
        switch (offset) {
        case 0x180U: return main_volume_left_;
        case 0x182U: return main_volume_right_;
        case 0x184U: return reverb_volume_left_;
        case 0x186U: return reverb_volume_right_;
        case 0x188U: return static_cast<std::uint16_t>(key_on_);
        case 0x18AU: return static_cast<std::uint16_t>(key_on_ >> 16U);
        case 0x18CU: return static_cast<std::uint16_t>(key_off_);
        case 0x18EU: return static_cast<std::uint16_t>(key_off_ >> 16U);
        case 0x190U: return static_cast<std::uint16_t>(pitch_modulation_);
        case 0x192U: return static_cast<std::uint16_t>(pitch_modulation_ >> 16U);
        case 0x194U: return static_cast<std::uint16_t>(noise_);
        case 0x196U: return static_cast<std::uint16_t>(noise_ >> 16U);
        case 0x198U: return static_cast<std::uint16_t>(reverb_);
        case 0x19AU: return static_cast<std::uint16_t>(reverb_ >> 16U);
        case 0x19CU: return static_cast<std::uint16_t>(voice_status_);
        case 0x19EU: return static_cast<std::uint16_t>(voice_status_ >> 16U);
        case 0x1A2U: return reverb_work_address_;
        case 0x1A4U: return irq_address_;
        case 0x1A6U: return transfer_address_register_;
        case 0x1AAU: return control_;
        case 0x1ACU: return transfer_control_;
        case 0x1AEU:
            // SPUSTAT's low bits mirror the control register's mode, and the
            // busy flags stay clear because every transfer here completes
            // before the guest can observe it.
            return static_cast<std::uint16_t>(control_ & 0x3FU);
        case 0x1B0U: return cd_volume_left_;
        case 0x1B2U: return cd_volume_right_;
        case 0x1B4U: return extern_volume_left_;
        case 0x1B6U: return extern_volume_right_;
        default: return 0U;
        }
    }
    const auto index = (offset - control_block_end) / 2U;
    return index < reverb_registers_.size() ? reverb_registers_[index] : 0U;
}

void Spu::writeRegister(std::uint32_t offset, std::uint16_t value) noexcept {
    if (offset < voice_block_end) {
        auto& voice = voices_[offset / 16U];
        switch (offset % 16U) {
        case 0x0U: voice.volume_left = value; return;
        case 0x2U: voice.volume_right = value; return;
        case 0x4U: voice.sample_rate = value; return;
        case 0x6U: voice.start_address = value; return;
        case 0x8U: voice.adsr_lo = value; return;
        case 0xAU: voice.adsr_hi = value; return;
        case 0xCU:
            // This register *is* the envelope level, not a copy of it. A game
            // that does not program the hardware ADSR -- and retail leaves every
            // ADSR register at zero -- drives its levels by writing here
            // instead, so a write has to move the envelope itself.
            voice.current_volume = value;
            voice_states_[offset / 16U].envelope =
                static_cast<std::int32_t>(value & 0x7FFFU);
            return;
        default:
            voice.repeat_address = value;
            // Writing the repeat address moves the loop point immediately, and
            // overrides whatever a block's loop-start flag had set. Retail
            // streams its music by pointing a playing voice at the half of the
            // buffer it has just refilled, so ignoring this register makes the
            // stream loop its opening instead of going on.
            voice_states_[offset / 16U].repeat_address = toByteAddress(value);
            return;
        }
    }
    if (offset >= control_block_end) {
        const auto index = (offset - control_block_end) / 2U;
        if (index < reverb_registers_.size()) {
            reverb_registers_[index] = value;
        }
        return;
    }
    switch (offset) {
    case 0x180U: main_volume_left_ = value; return;
    case 0x182U: main_volume_right_ = value; return;
    case 0x184U: reverb_volume_left_ = value; return;
    case 0x186U: reverb_volume_right_ = value; return;
    // Key on and key off are edges. Retail writes the halves independently, so
    // each write latches only the voices it names and the pending set is
    // cleared by whoever consumes it, never by a later write.
    case 0x188U:
    case 0x18AU: {
        const auto shift = offset == 0x188U ? 0U : 16U;
        const auto voices = static_cast<std::uint32_t>(value) << shift;
        key_on_ |= voices;
        voice_status_ |= voices;
        key_on_count_ += static_cast<std::uint64_t>(std::popcount(voices));
        // Applied here rather than at the next mix, because hardware starts the
        // voice when the register is written and everything the guest writes
        // afterwards must survive. Retail points a stream's repeat address at
        // the half it has just refilled after keying it, and deferring the
        // key-on would reset that write instead of honouring it.
        for (std::uint32_t index = 0U; index < voice_count; ++index) {
            if ((voices & (1U << index)) == 0U) {
                continue;
            }
            auto& state = voice_states_[index];
            state = VoiceState{};
            state.active = true;
            state.phase = AdsrPhase::attack;
            state.block_address = toByteAddress(voices_[index].start_address);
            state.repeat_address = state.block_address;
        }
        return;
    }
    case 0x18CU:
    case 0x18EU: {
        const auto shift = offset == 0x18CU ? 0U : 16U;
        const auto voices = static_cast<std::uint32_t>(value) << shift;
        key_off_ |= voices;
        voice_status_ &= ~voices;
        key_off_count_ += static_cast<std::uint64_t>(std::popcount(voices));
        for (std::uint32_t index = 0U; index < voice_count; ++index) {
            if ((voices & (1U << index)) != 0U &&
                voice_states_[index].active) {
                // A key-off releases; the voice is finished only when the
                // release envelope reaches zero. Stopping it here instead is
                // what made every note end abruptly.
                voice_states_[index].phase = AdsrPhase::release;
            }
        }
        return;
    }
    case 0x190U:
        pitch_modulation_ = (pitch_modulation_ & 0xFFFF0000U) | value;
        return;
    case 0x192U:
        pitch_modulation_ =
            (pitch_modulation_ & 0xFFFFU) |
            (static_cast<std::uint32_t>(value) << 16U);
        return;
    case 0x194U: noise_ = (noise_ & 0xFFFF0000U) | value; return;
    case 0x196U:
        noise_ = (noise_ & 0xFFFFU) | (static_cast<std::uint32_t>(value) << 16U);
        return;
    case 0x198U: reverb_ = (reverb_ & 0xFFFF0000U) | value; return;
    case 0x19AU:
        reverb_ =
            (reverb_ & 0xFFFFU) | (static_cast<std::uint32_t>(value) << 16U);
        return;
    case 0x1A2U:
        reverb_work_address_ = value;
        reverb_current_address_ =
            (static_cast<std::uint32_t>(value) << 2U) & reverb_address_mask;
        return;
    case 0x1A4U: irq_address_ = value; return;
    case 0x1A6U:
        // Writing the transfer address also reloads the running destination,
        // which is what makes a following DMA land where the guest asked.
        transfer_address_register_ = value;
        transfer_address_ = toByteAddress(value);
        return;
    case 0x1A8U:
        // The manual transfer FIFO. Retail uses DMA4 for bulk uploads, but the
        // port has to work or a small write silently vanishes.
        if (transfer_address_ + 1U < sound_ram_bytes) {
            sound_ram_[transfer_address_] =
                static_cast<std::uint8_t>(value & 0xFFU);
            sound_ram_[transfer_address_ + 1U] =
                static_cast<std::uint8_t>(value >> 8U);
            transfer_address_ += 2U;
            sound_ram_bytes_written_ += 2U;
        }
        return;
    case 0x1AAU: control_ = value; return;
    case 0x1ACU: transfer_control_ = value; return;
    case 0x1B0U: cd_volume_left_ = value; return;
    case 0x1B2U: cd_volume_right_ = value; return;
    case 0x1B4U: extern_volume_left_ = value; return;
    case 0x1B6U: extern_volume_right_ = value; return;
    default: return;
    }
}

void Spu::decodeBlock(std::uint32_t index) noexcept {
    // The ADPCM predictor's coefficients, as sixty-fourths.
    constexpr std::array<std::int32_t, 5U> filter_0{0, 60, 115, 98, 122};
    constexpr std::array<std::int32_t, 5U> filter_1{0, 0, -52, -55, -60};

    auto& state = voice_states_[index];
    const auto base = state.block_address & (sound_ram_bytes - 1U);
    // Playback reaching the IRQ address is what tells retail to refill the half
    // of its streaming buffer that is not playing. Hardware compares every
    // sample; a block is close enough, because the address retail sets is a
    // buffer boundary rather than a point inside one.
    if (irqEnabled() &&
        (base >> 4U) == ((irqAddress() & (sound_ram_bytes - 1U)) >> 4U)) {
        irq_pending_ = true;
        ++irq_count_;
    }
    const auto header = sound_ram_[base];
    state.flags = sound_ram_[(base + 1U) & (sound_ram_bytes - 1U)];

    auto shift = static_cast<std::uint32_t>(header & 0x0FU);
    // Shifts above twelve are undefined on hardware and behave as nine.
    if (shift > 12U) {
        shift = 9U;
    }
    const auto filter = std::min<std::uint32_t>(
        (header >> 4U) & 0x07U,
        static_cast<std::uint32_t>(filter_0.size() - 1U));

    // A loop-start flag names this block as the repeat point.
    if ((state.flags & 0x04U) != 0U) {
        state.repeat_address = state.block_address;
    }

    for (std::uint32_t nibble = 0U; nibble < 28U; ++nibble) {
        const auto byte =
            sound_ram_[(base + 2U + nibble / 2U) & (sound_ram_bytes - 1U)];
        auto raw = static_cast<std::int32_t>(
            (nibble % 2U) != 0U ? (byte >> 4U) : (byte & 0x0FU));
        // Four-bit two's complement, then scaled into sixteen bits.
        if (raw > 7) {
            raw -= 16;
        }
        auto sample = raw << (12U - shift);
        sample += (state.history1 * filter_0[filter] +
                   state.history2 * filter_1[filter]) /
            64;
        sample = std::clamp(sample, -32768, 32767);
        state.samples[nibble] = static_cast<std::int16_t>(sample);
        state.history2 = state.history1;
        state.history1 = sample;
    }
    state.decoded = true;
}

void Spu::tickEnvelope(std::uint32_t index) noexcept {
    auto& state = voice_states_[index];
    if (state.phase == AdsrPhase::off) {
        return;
    }
    const auto lo = voices_[index].adsr_lo;
    const auto hi = voices_[index].adsr_hi;
    // Decay ends the moment the sustain level is reached, checked before
    // stepping rather than after. Checking afterwards applies one more decay
    // step first, which drops the envelope below the level the guest asked to
    // sustain at -- audible as every sustained note starting a notch too quiet.
    if (state.phase == AdsrPhase::decay) {
        const auto sustain_level =
            (static_cast<std::int32_t>(lo & 0x0FU) + 1) * 0x800;
        if (state.envelope <= sustain_level) {
            state.phase = AdsrPhase::sustain;
        }
    }

    std::uint32_t shift = 0U;
    std::uint32_t step_index = 0U;
    auto decreasing = false;
    auto exponential = false;
    switch (state.phase) {
    case AdsrPhase::attack:
        shift = (lo >> 10U) & 0x1FU;
        step_index = (lo >> 8U) & 0x03U;
        exponential = (lo & 0x8000U) != 0U;
        break;
    case AdsrPhase::decay:
        shift = (lo >> 4U) & 0x0FU;
        // Decay's step is fixed and its curve is always exponential.
        decreasing = true;
        exponential = true;
        break;
    case AdsrPhase::sustain:
        shift = (hi >> 8U) & 0x1FU;
        step_index = (hi >> 6U) & 0x03U;
        decreasing = (hi & 0x4000U) != 0U;
        exponential = (hi & 0x8000U) != 0U;
        break;
    default:
        shift = hi & 0x1FU;
        decreasing = true;
        exponential = (hi & 0x0020U) != 0U;
        break;
    }

    // A shift above eleven spreads one step over several ticks; below eleven it
    // scales the step up instead. Decreasing steps run -8..-5, increasing +7..+4.
    const auto fixed_step = state.phase == AdsrPhase::decay ||
        state.phase == AdsrPhase::release;
    const auto base = decreasing
        ? -(8 - static_cast<std::int32_t>(fixed_step ? 0U : step_index))
        : (7 - static_cast<std::int32_t>(step_index));
    auto cycles = 1U
        << static_cast<std::uint32_t>(
               std::max(0, static_cast<std::int32_t>(shift) - 11));
    auto step = base *
        (1 << static_cast<std::uint32_t>(
             std::max(0, 11 - static_cast<std::int32_t>(shift))));
    if (exponential) {
        if (!decreasing && state.envelope > 0x6000) {
            // An exponential rise flattens out near the top.
            cycles *= 4U;
        }
        if (decreasing) {
            step = static_cast<std::int32_t>(
                static_cast<std::int64_t>(step) * state.envelope / 0x8000);
        }
    }

    if (++state.envelope_counter < cycles) {
        return;
    }
    state.envelope_counter = 0U;
    state.envelope = std::clamp(state.envelope + step, 0, 0x7FFF);

    switch (state.phase) {
    case AdsrPhase::attack:
        if (state.envelope >= 0x7FFF) {
            state.phase = AdsrPhase::decay;
        }
        break;
    case AdsrPhase::decay: {
        const auto sustain_level =
            (static_cast<std::int32_t>(lo & 0x0FU) + 1) * 0x800;
        if (state.envelope <= sustain_level) {
            state.phase = AdsrPhase::sustain;
        }
        break;
    }
    case AdsrPhase::release:
        if (state.envelope <= 0) {
            // Only now is the voice finished.
            state.phase = AdsrPhase::off;
            state.active = false;
        }
        break;
    default:
        break;
    }
}

std::int32_t Spu::nextVoiceSample(std::uint32_t index) noexcept {
    auto& state = voice_states_[index];
    if (!state.active) {
        return 0;
    }
    if (!state.decoded) {
        decodeBlock(index);
    }
    const auto raw = static_cast<std::int32_t>(
        state.samples[std::min<std::uint32_t>(state.sample_index, 27U)]);
    const auto sample = static_cast<std::int32_t>(
        static_cast<std::int64_t>(raw) * state.envelope / 0x8000);
    tickEnvelope(index);

    // The pitch register is a twelve-bit ratio; hardware clamps it to four.
    constexpr std::uint32_t pitch_unit = 0x1000U;
    const auto pitch =
        std::min<std::uint32_t>(voices_[index].sample_rate, 0x3FFFU);
    state.pitch_counter += pitch;
    while (state.pitch_counter >= pitch_unit) {
        state.pitch_counter -= pitch_unit;
        ++state.sample_index;
        if (state.sample_index < 28U) {
            continue;
        }
        state.sample_index = 0U;
        if ((state.flags & 0x01U) != 0U) {
            // Loop end: go back to the repeat point, and stop unless the block
            // also asked to repeat.
            state.block_address = state.repeat_address;
            if ((state.flags & 0x02U) == 0U) {
                // Loop end without repeat silences the voice on hardware, which
                // is a release from zero rather than a cut.
                state.envelope = 0;
                state.phase = AdsrPhase::off;
                state.active = false;
                return sample;
            }
        } else {
            state.block_address += 16U;
        }
        state.decoded = false;
        decodeBlock(index);
    }
    return sample;
}

std::uint32_t Spu::reverbMemoryAddress(std::uint32_t address) const noexcept {
    const auto base =
        (static_cast<std::uint32_t>(reverb_work_address_) << 2U) &
        reverb_address_mask;
    auto offset = reverb_current_address_ + (address & reverb_address_mask);
    if ((offset & (reverb_address_mask + 1U)) != 0U) {
        offset += base;
    }
    return (offset & reverb_address_mask) * 2U;
}

std::int16_t Spu::reverbRead(
    std::uint16_t address, std::int32_t offset) const noexcept {
    const auto scaled = (static_cast<std::uint32_t>(address) << 2U) +
        static_cast<std::uint32_t>(offset);
    const auto real_address = reverbMemoryAddress(scaled);
    const auto bits = static_cast<std::uint16_t>(
        sound_ram_[real_address] |
        (static_cast<std::uint16_t>(sound_ram_[real_address + 1U]) << 8U));
    return std::bit_cast<std::int16_t>(bits);
}

void Spu::reverbWrite(std::uint16_t address, std::int16_t value) noexcept {
    const auto real_address =
        reverbMemoryAddress(static_cast<std::uint32_t>(address) << 2U);
    const auto bits = std::bit_cast<std::uint16_t>(value);
    sound_ram_[real_address] = static_cast<std::uint8_t>(bits);
    sound_ram_[real_address + 1U] = static_cast<std::uint8_t>(bits >> 8U);
}

std::array<std::int32_t, 2U> Spu::processReverb(
    std::int32_t left, std::int32_t right) noexcept {
    const auto position = static_cast<std::size_t>(reverb_resample_position_);
    reverb_downsample_buffer_[0U][position] =
        reverb_downsample_buffer_[0U][position | 0x40U] = clampSample(left);
    reverb_downsample_buffer_[1U][position] =
        reverb_downsample_buffer_[1U][position | 0x40U] = clampSample(right);

    const auto config = [this](std::size_t index) noexcept {
        return reverb_registers_[index];
    };
    const auto signed_config = [&config](std::size_t index) noexcept {
        return std::bit_cast<std::int16_t>(config(index));
    };
    const auto shifted = [](std::int64_t value, std::uint32_t amount) noexcept {
        return floorDivPowerOfTwo(value, amount);
    };
    const auto negative = [](std::int32_t value) noexcept {
        return value == std::numeric_limits<std::int16_t>::min()
            ? static_cast<std::int32_t>(
                  std::numeric_limits<std::int16_t>::max())
            : -value;
    };

    std::array<std::int32_t, 2U> output{};
    if ((position & 1U) != 0U) {
        std::array<std::int32_t, 2U> downsampled{};
        for (std::size_t channel = 0U; channel < 2U; ++channel) {
            const auto start = (position - 38U) & 0x3FU;
            std::int64_t sum = static_cast<std::int64_t>(0x4000) *
                reverb_downsample_buffer_[channel][start + 19U];
            for (std::size_t tap = 0U;
                 tap < reverb_resample_coefficients.size(); ++tap) {
                sum += static_cast<std::int64_t>(
                           reverb_resample_coefficients[tap]) *
                    reverb_downsample_buffer_[channel][start + tap * 2U];
            }
            downsampled[channel] = clampSample(shifted(sum, 15U));
        }

        const auto reverb_enabled =
            (control_ & control_reverb_enable) != 0U;
        const auto iir_alpha = static_cast<std::int32_t>(signed_config(2U));
        const auto iir_adjust = [iir_alpha](std::int16_t sample) noexcept {
            if (iir_alpha == std::numeric_limits<std::int16_t>::min()) {
                return sample == std::numeric_limits<std::int16_t>::min()
                    ? std::int64_t{0}
                    : static_cast<std::int64_t>(sample) * -65536;
            }
            return static_cast<std::int64_t>(sample) * (32768 - iir_alpha);
        };

        for (std::size_t channel = 0U; channel < 2U; ++channel) {
            if (reverb_enabled) {
                const auto input_a = clampSample(shifted(
                    shifted(
                        static_cast<std::int64_t>(
                            reverbRead(config(16U + channel))) *
                            signed_config(7U),
                        14U) +
                        shifted(
                            static_cast<std::int64_t>(downsampled[channel]) *
                                signed_config(30U + channel),
                            14U),
                    1U));
                const auto input_b = clampSample(shifted(
                    shifted(
                        static_cast<std::int64_t>(reverbRead(
                            config(24U + (channel ^ 1U)))) *
                            signed_config(7U),
                        14U) +
                        shifted(
                            static_cast<std::int64_t>(downsampled[channel]) *
                                signed_config(30U + channel),
                            14U),
                    1U));
                const auto iir_a = clampSample(shifted(
                    shifted(
                        static_cast<std::int64_t>(input_a) * iir_alpha,
                        14U) +
                        shifted(
                            iir_adjust(
                                reverbRead(config(10U + channel), -1)),
                            14U),
                    1U));
                const auto iir_b = clampSample(shifted(
                    shifted(
                        static_cast<std::int64_t>(input_b) * iir_alpha,
                        14U) +
                        shifted(
                            iir_adjust(
                                reverbRead(config(18U + channel), -1)),
                            14U),
                    1U));
                reverbWrite(config(10U + channel), iir_a);
                reverbWrite(config(18U + channel), iir_b);
            }

            auto accumulator = shifted(
                static_cast<std::int64_t>(
                    reverbRead(config(12U + channel))) *
                    signed_config(3U),
                14U);
            accumulator += shifted(
                static_cast<std::int64_t>(
                    reverbRead(config(14U + channel))) *
                    signed_config(4U),
                14U);
            accumulator += shifted(
                static_cast<std::int64_t>(
                    reverbRead(config(20U + channel))) *
                    signed_config(5U),
                14U);
            accumulator += shifted(
                static_cast<std::int64_t>(
                    reverbRead(config(22U + channel))) *
                    signed_config(6U),
                14U);

            const auto feedback_a = reverbRead(static_cast<std::uint16_t>(
                config(26U + channel) - config(0U)));
            const auto feedback_b = reverbRead(static_cast<std::uint16_t>(
                config(28U + channel) - config(1U)));
            const auto mix_a = clampSample(shifted(
                accumulator +
                    shifted(
                        static_cast<std::int64_t>(feedback_a) *
                            negative(signed_config(8U)),
                        14U),
                1U));
            const auto mix_b = clampSample(
                static_cast<std::int64_t>(feedback_a) +
                shifted(
                    shifted(
                        static_cast<std::int64_t>(mix_a) *
                            signed_config(8U),
                        14U) +
                        shifted(
                            static_cast<std::int64_t>(feedback_b) *
                                negative(signed_config(9U)),
                            14U),
                    1U));
            const auto upsampled = clampSample(
                static_cast<std::int64_t>(feedback_b) +
                shifted(
                    static_cast<std::int64_t>(mix_b) * signed_config(9U),
                    15U));
            const auto upsample_position = position >> 1U;
            reverb_upsample_buffer_[channel][upsample_position] =
                reverb_upsample_buffer_[channel][upsample_position | 0x20U] =
                    upsampled;
            if (reverb_enabled) {
                reverbWrite(config(26U + channel), mix_a);
                reverbWrite(config(28U + channel), mix_b);
            }
        }

        const auto base =
            (static_cast<std::uint32_t>(reverb_work_address_) << 2U) &
            reverb_address_mask;
        reverb_current_address_ =
            (reverb_current_address_ + 1U) & reverb_address_mask;
        if (reverb_current_address_ == 0U) {
            reverb_current_address_ = base;
        }

        for (std::size_t channel = 0U; channel < 2U; ++channel) {
            const auto start = ((position >> 1U) - 19U) & 0x1FU;
            std::int64_t sum{};
            for (std::size_t tap = 0U;
                 tap < reverb_resample_coefficients.size(); ++tap) {
                sum += static_cast<std::int64_t>(
                           reverb_resample_coefficients[tap]) *
                    reverb_upsample_buffer_[channel][start + tap];
            }
            output[channel] = clampSample(shifted(sum, 14U));
        }
    } else {
        const auto index = (((position >> 1U) - 19U) & 0x1FU) + 9U;
        output[0U] = reverb_upsample_buffer_[0U][index];
        output[1U] = reverb_upsample_buffer_[1U][index];
    }

    reverb_resample_position_ =
        static_cast<std::uint8_t>((position + 1U) & 0x3FU);
    const auto left_volume = std::bit_cast<std::int16_t>(reverb_volume_left_);
    const auto right_volume = std::bit_cast<std::int16_t>(reverb_volume_right_);
    return {
        clampSample(shifted(
            static_cast<std::int64_t>(output[0U]) * left_volume, 15U)),
        clampSample(shifted(
            static_cast<std::int64_t>(output[1U]) * right_volume, 15U)),
    };
}

std::uint32_t Spu::activeVoices() const noexcept {
    return static_cast<std::uint32_t>(
        std::popcount(activeVoiceMask()));
}

std::uint32_t Spu::activeVoiceMask() const noexcept {
    std::uint32_t active = 0U;
    for (std::uint32_t index = 0U; index < voice_count; ++index) {
        if (voice_states_[index].active) {
            active |= 1U << index;
        }
    }
    return active;
}

void Spu::mix(std::span<std::int16_t> out) noexcept {
    // A volume register is a signed fifteen-bit level in its low bits, doubled.
    // Bit fifteen selects a sweep instead, whose envelope is not modelled yet;
    // holding it at full scale keeps those voices audible rather than silent.
    const auto level = [](std::uint16_t reg) -> std::int32_t {
        if ((reg & 0x8000U) != 0U) {
            return 0x7FFF;
        }
        return static_cast<std::int16_t>(static_cast<std::uint16_t>(reg << 1U));
    };
    const auto main_left = level(main_volume_left_);
    const auto main_right = level(main_volume_right_);

    for (std::size_t frame = 0U; frame + 1U < out.size(); frame += 2U) {
        std::int64_t left = 0;
        std::int64_t right = 0;
        std::int64_t reverb_left = 0;
        std::int64_t reverb_right = 0;
        for (std::uint32_t index = 0U; index < voice_count; ++index) {
            const auto sample = nextVoiceSample(index);
            if (sample == 0) {
                continue;
            }
            const auto voice_left = static_cast<std::int64_t>(sample) *
                level(voices_[index].volume_left) / 0x8000;
            const auto voice_right = static_cast<std::int64_t>(sample) *
                level(voices_[index].volume_right) / 0x8000;
            left += voice_left;
            right += voice_right;
            if ((reverb_ & (1U << index)) != 0U) {
                // Reverb routing is a send: the voice remains in the dry mix
                // while a copy also enters the effect unit.
                reverb_left += voice_left;
                reverb_right += voice_right;
            }
        }
        const auto wet = processReverb(
            clampSample(reverb_left), clampSample(reverb_right));
        left += wet[0U];
        right += wet[1U];
        left = left * main_left / 0x8000;
        right = right * main_right / 0x8000;
        out[frame] = clampSample(left);
        out[frame + 1U] = clampSample(right);
    }
}

void Spu::advance(std::uint64_t frames) noexcept {
    constexpr std::size_t frames_per_block = 735U;
    std::array<std::int16_t, frames_per_block * 2U> discarded{};
    while (frames != 0U) {
        const auto block_frames = static_cast<std::size_t>(
            std::min<std::uint64_t>(frames, frames_per_block));
        mix(std::span{discarded}.first(block_frames * 2U));
        frames -= block_frames;
    }
}

void Spu::writeSoundRam(std::span<const std::uint16_t> words) noexcept {
    for (const auto word : words) {
        if (transfer_address_ + 1U >= sound_ram_bytes) {
            // Sound RAM wraps rather than faulting.
            transfer_address_ = 0U;
        }
        sound_ram_[transfer_address_] = static_cast<std::uint8_t>(word & 0xFFU);
        sound_ram_[transfer_address_ + 1U] =
            static_cast<std::uint8_t>(word >> 8U);
        transfer_address_ += 2U;
        sound_ram_bytes_written_ += 2U;
    }
}

} // namespace stuntmaster::psx
