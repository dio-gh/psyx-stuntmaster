#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__STUNTMASTER_PSX
#include "stuntmaster_xa.c"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

using Payload =
    std::array<std::uint8_t, WUFFS_STUNTMASTER_PSX__XA_PAYLOAD_BYTES>;
using Output =
    std::array<std::uint8_t, WUFFS_STUNTMASTER_PSX__XA_OUTPUT_BYTES>;

Payload silentPayload() {
    Payload payload{};
    for (std::size_t group = 0; group < 18; ++group) {
        std::fill_n(payload.begin() + static_cast<std::ptrdiff_t>(group * 128),
                    16, std::uint8_t{0x0C});
    }
    return payload;
}

void setStereoParameter(
    Payload& payload,
    std::size_t group,
    std::size_t block,
    std::uint8_t left,
    std::uint8_t right) {
    const auto base = group * 128;
    const auto left_index = base + 4 + block * 2;
    const auto right_index = left_index + 1;
    payload[left_index] = left;
    payload[right_index] = right;
    if (block < 2) {
        payload[left_index - 4] = left;
        payload[right_index - 4] = right;
    } else {
        payload[left_index + 4] = left;
        payload[right_index + 4] = right;
    }
}

std::int16_t sample(const Output& output, std::size_t frame, std::size_t channel) {
    const auto index = (frame * 2 + channel) * 2;
    const auto bits = static_cast<std::uint16_t>(output[index]) |
        (static_cast<std::uint16_t>(output[index + 1]) << 8);
    std::int16_t result{};
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

wuffs_base__status decode(
    wuffs_stuntmaster_psx__xa_decoder* decoder,
    Output& output,
    Payload& payload,
    std::uint8_t coding_info) {
    return wuffs_stuntmaster_psx__xa_decoder__decode_sector(
        decoder,
        wuffs_base__make_slice_u8(output.data(), output.size()),
        wuffs_base__make_slice_u8(payload.data(), payload.size()),
        coding_info);
}

void assertError(wuffs_base__status status, const char* message) {
    assert(status.is_error());
    assert(status.message() != nullptr);
    assert(std::strcmp(status.message(), message) == 0);
}

void supportedRatesAndSilence() {
    auto* decoder = wuffs_stuntmaster_psx__xa_decoder__alloc();
    assert(decoder != nullptr);
    assert(wuffs_stuntmaster_psx__xa_decoder__sample_rate(decoder, 0x01) ==
           37800);
    assert(wuffs_stuntmaster_psx__xa_decoder__sample_rate(decoder, 0x05) ==
           18900);
    assert(wuffs_stuntmaster_psx__xa_decoder__sample_rate(decoder, 0x00) == 0);

    auto payload = silentPayload();
    Output output;
    output.fill(0xA5);
    const auto status = decode(decoder, output, payload, 0x05);
    assert(status.is_ok());
    for (const auto byte : output) {
        assert(byte == 0);
    }
    std::free(decoder);
}

void stereoNibbleOrderAndSignedResiduals() {
    auto* decoder = wuffs_stuntmaster_psx__xa_decoder__alloc();
    assert(decoder != nullptr);
    auto payload = silentPayload();
    // group 0, block 0, filter 0, range 12: residuals are the signed
    // nibble values themselves. Low nibble is left, high nibble is right.
    payload[16] = 0x71;
    payload[20] = 0x8F;
    payload[24] = 0x27;

    Output output{};
    const auto status = decode(decoder, output, payload, 0x01);
    assert(status.is_ok());
    assert(sample(output, 0, 0) == 1);
    assert(sample(output, 0, 1) == 7);
    assert(sample(output, 1, 0) == -1);
    assert(sample(output, 1, 1) == -8);
    assert(sample(output, 2, 0) == 7);
    assert(sample(output, 2, 1) == 2);
    assert(sample(output, 3, 0) == 0);
    assert(sample(output, 3, 1) == 0);
    std::free(decoder);
}

void predictorHistoryContinuesAcrossSectors() {
    auto* decoder = wuffs_stuntmaster_psx__xa_decoder__alloc();
    assert(decoder != nullptr);
    auto first = silentPayload();
    // Seed both channel histories with two full-scale-ish residuals in the
    // final sound unit of a sector.
    setStereoParameter(first, 17, 3, 0x00, 0x00);
    first[17 * 128 + 16 + 3 + 26 * 4] = 0x87;
    first[17 * 128 + 16 + 3 + 27 * 4] = 0x87;
    Output output{};
    auto status = decode(decoder, output, first, 0x05);
    assert(status.is_ok());
    assert(sample(output, 2014, 0) == 28672);
    assert(sample(output, 2014, 1) == -32768);
    assert(sample(output, 2015, 0) == 28672);
    assert(sample(output, 2015, 1) == -32768);

    auto second = silentPayload();
    setStereoParameter(second, 0, 0, 0x1C, 0x1C);
    output.fill(0);
    status = decode(decoder, output, second, 0x05);
    assert(status.is_ok());
    assert(sample(output, 0, 0) == 26880);
    assert(sample(output, 0, 1) == -30720);
    assert(sample(output, 1, 0) == 25200);
    assert(sample(output, 1, 1) == -28800);

    wuffs_stuntmaster_psx__xa_decoder__reset_history(decoder);
    output.fill(0xA5);
    status = decode(decoder, output, second, 0x05);
    assert(status.is_ok());
    assert(sample(output, 0, 0) == 0);
    assert(sample(output, 0, 1) == 0);
    std::free(decoder);
}

void malformedInputsFailClosed() {
    Output output{};
    auto payload = silentPayload();

    {
        auto* decoder = wuffs_stuntmaster_psx__xa_decoder__alloc();
        assert(decoder != nullptr);
        const auto status = wuffs_stuntmaster_psx__xa_decoder__decode_sector(
            decoder,
            wuffs_base__make_slice_u8(output.data(), output.size()),
            wuffs_base__make_slice_u8(payload.data(), payload.size() - 1),
            0x05);
        assertError(status, "stuntmaster_psx: short XA input");
        std::free(decoder);
    }
    {
        auto* decoder = wuffs_stuntmaster_psx__xa_decoder__alloc();
        assert(decoder != nullptr);
        const auto status = wuffs_stuntmaster_psx__xa_decoder__decode_sector(
            decoder,
            wuffs_base__make_slice_u8(output.data(), output.size() - 1),
            wuffs_base__make_slice_u8(payload.data(), payload.size()),
            0x05);
        assertError(status, "stuntmaster_psx: short XA output");
        std::free(decoder);
    }
    {
        auto* decoder = wuffs_stuntmaster_psx__xa_decoder__alloc();
        assert(decoder != nullptr);
        auto status = decode(decoder, output, payload, 0x45);
        assertError(status, "stuntmaster_psx: bad XA coding info");
        std::free(decoder);
    }
    {
        auto* decoder = wuffs_stuntmaster_psx__xa_decoder__alloc();
        assert(decoder != nullptr);
        auto damaged = payload;
        damaged[0] ^= 1;
        auto status = decode(decoder, output, damaged, 0x05);
        assertError(
            status, "stuntmaster_psx: inconsistent XA parameter copies");
        std::free(decoder);
    }
    for (const auto parameter : {std::uint8_t{0x0D}, std::uint8_t{0x4C}}) {
        auto* decoder = wuffs_stuntmaster_psx__xa_decoder__alloc();
        assert(decoder != nullptr);
        auto invalid = payload;
        setStereoParameter(invalid, 0, 0, parameter, parameter);
        auto status = decode(decoder, output, invalid, 0x05);
        assertError(status, "stuntmaster_psx: bad XA parameter");
        std::free(decoder);
    }
}

void lateFailureIsAtomic() {
    auto* tested = wuffs_stuntmaster_psx__xa_decoder__alloc();
    auto* clean = wuffs_stuntmaster_psx__xa_decoder__alloc();
    assert(tested != nullptr);
    assert(clean != nullptr);

    auto first = silentPayload();
    setStereoParameter(first, 17, 3, 0x00, 0x00);
    first[17 * 128 + 16 + 3 + 26 * 4] = 0x87;
    first[17 * 128 + 16 + 3 + 27 * 4] = 0x87;
    Output tested_output{};
    Output clean_output{};
    assert(decode(tested, tested_output, first, 0x05).is_ok());
    assert(decode(clean, clean_output, first, 0x05).is_ok());

    auto invalid = silentPayload();
    setStereoParameter(invalid, 17, 3, 0x4C, 0x4C);
    tested_output.fill(0xA5);
    assertError(
        decode(tested, tested_output, invalid, 0x05),
        "stuntmaster_psx: bad XA parameter");
    assert(std::all_of(tested_output.begin(), tested_output.end(),
                       [](std::uint8_t byte) { return byte == 0xA5; }));

    auto next = silentPayload();
    setStereoParameter(next, 0, 0, 0x1C, 0x1C);
    assert(decode(tested, tested_output, next, 0x05).is_ok());
    assert(decode(clean, clean_output, next, 0x05).is_ok());
    assert(tested_output == clean_output);

    std::free(tested);
    std::free(clean);
}

} // namespace

int main() {
    supportedRatesAndSilence();
    stereoNibbleOrderAndSignedResiduals();
    predictorHistoryContinuesAcrossSectors();
    malformedInputsFailClosed();
    lateFailureIsAtomic();
    return 0;
}
