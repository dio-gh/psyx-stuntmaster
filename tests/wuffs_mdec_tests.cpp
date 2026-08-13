#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__STUNTMASTER_PSX
#include "stuntmaster_mdec.c"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::size_t kBlocks = 20U * 15U * 6U;
constexpr std::size_t kRunLevelHalfwords = kBlocks * 2U;
constexpr std::size_t kRunLevelWords =
    ((kRunLevelHalfwords + 63U) / 64U) * 32U;
constexpr std::size_t kPayloadBits = kBlocks * 12U + 10U;
constexpr std::size_t kBitstreamBytes = ((kPayloadBits + 31U) / 32U) * 4U;
constexpr std::size_t kFrameBytes = 8U + kBitstreamBytes;
constexpr std::size_t kRgbaBytes = 320U * 240U * 4U;

void putBit(
    std::vector<std::uint8_t>& logical,
    std::size_t position,
    bool value) {
    if (value) {
        logical[position / 8U] |= static_cast<std::uint8_t>(
            0x80U >> (position % 8U));
    }
}

void putBits(
    std::vector<std::uint8_t>& logical,
    std::size_t& position,
    std::uint32_t value,
    int count) {
    for (int bit = count - 1; bit >= 0; --bit) {
        putBit(logical, position++, ((value >> bit) & 1U) != 0U);
    }
}

std::vector<std::uint8_t> constantFrame(
    std::int16_t luminance_dc,
    std::int16_t cr_dc = 0,
    std::int16_t cb_dc = 0) {
    std::vector<std::uint8_t> logical(kBitstreamBytes);
    std::size_t position = 0;
    for (std::size_t block = 0; block < kBlocks; ++block) {
        const auto position_in_macroblock = block % 6U;
        const auto dc = position_in_macroblock == 0U ? cr_dc :
            (position_in_macroblock == 1U ? cb_dc : luminance_dc);
        const auto dc_bits = static_cast<std::uint16_t>(dc) & 0x03FFU;
        for (int bit = 9; bit >= 0; --bit) {
            putBit(logical, position++, ((dc_bits >> bit) & 1U) != 0U);
        }
        putBit(logical, position++, true);  // EOB is 10.
        putBit(logical, position++, false);
    }
    for (int bit = 9; bit >= 0; --bit) {
        putBit(logical, position++, ((0x1FFU >> bit) & 1U) != 0U);
    }
    assert(logical.size() * 8U - position <= 31U);

    std::vector<std::uint8_t> frame(kFrameBytes);
    frame[0] = static_cast<std::uint8_t>(kRunLevelWords & 0xFFU);
    frame[1] = static_cast<std::uint8_t>(kRunLevelWords >> 8U);
    frame[2] = 0x00;    // BS magic 0x3800, little-endian.
    frame[3] = 0x38;
    frame[4] = 1;       // qscale.
    frame[6] = 2;       // BS version 2.
    for (std::size_t i = 0; i < logical.size(); ++i) {
        // Sony BS stores MSB-first MPEG bits in little-endian halfwords.
        frame[8U + (i ^ 1U)] = logical[i];
    }
    return frame;
}

std::vector<std::uint8_t> singleAcFrame(bool escape_coded) {
    constexpr std::size_t kLumaBlocks = 20U * 15U * 4U;
    const auto payload_bits = kBlocks * 12U +
        kLumaBlocks * (escape_coded ? 22U : 5U) + 10U;
    const auto bitstream_bytes = ((payload_bits + 31U) / 32U) * 4U;
    std::vector<std::uint8_t> logical(bitstream_bytes);
    std::size_t position = 0;
    std::size_t halfwords = 0;
    for (std::size_t block = 0; block < kBlocks; ++block) {
        putBits(logical, position, 0, 10);  // Neutral DC.
        ++halfwords;
        if ((block % 6U) >= 2U) {
            if (escape_coded) {
                putBits(logical, position, 1, 6);  // 000001 escape.
                putBits(logical, position, 2, 16); // Run zero, level +2.
            } else {
                putBits(logical, position, 4, 4);  // VLC 0100s.
                putBits(logical, position, 0, 1);  // Positive sign.
            }
            ++halfwords;
        }
        putBits(logical, position, 2, 2);  // EOB 10.
        ++halfwords;
    }
    putBits(logical, position, 0x1FFU, 10);
    assert(logical.size() * 8U - position <= 31U);

    std::vector<std::uint8_t> frame(8U + bitstream_bytes);
    const auto run_level_words = ((halfwords + 63U) / 64U) * 32U;
    frame[0] = static_cast<std::uint8_t>(run_level_words & 0xFFU);
    frame[1] = static_cast<std::uint8_t>(run_level_words >> 8U);
    frame[2] = 0x00;
    frame[3] = 0x38;
    frame[4] = 1;
    frame[6] = 2;
    for (std::size_t i = 0; i < logical.size(); ++i) {
        frame[8U + (i ^ 1U)] = logical[i];
    }
    return frame;
}

std::vector<std::uint8_t> idctRoundingFrame() {
    constexpr std::size_t kLumaBlocks = 20U * 15U * 4U;
    const auto payload_bits = kBlocks * 12U + kLumaBlocks * 18U + 10U;
    const auto bitstream_bytes = ((payload_bits + 31U) / 32U) * 4U;
    std::vector<std::uint8_t> logical(bitstream_bytes);
    std::size_t position = 0;
    std::size_t halfwords = 0;
    for (std::size_t block = 0; block < kBlocks; ++block) {
        const bool luma = (block % 6U) >= 2U;
        putBits(logical, position, luma ? (static_cast<std::uint16_t>(-83) & 0x3FFU) : 0U, 10);
        ++halfwords;
        if (luma) {
            // Two run-zero, level -6 table codes (00100 001 sign), producing
            // natural coefficients [1] = -12 and [8] = -12 at qscale 1.
            putBits(logical, position, 0x43, 9);
            putBits(logical, position, 0x43, 9);
            halfwords += 2U;
        }
        putBits(logical, position, 2, 2);
        ++halfwords;
    }
    putBits(logical, position, 0x1FFU, 10);
    assert(logical.size() * 8U - position <= 31U);

    std::vector<std::uint8_t> frame(8U + bitstream_bytes);
    const auto run_level_words = ((halfwords + 63U) / 64U) * 32U;
    frame[0] = static_cast<std::uint8_t>(run_level_words & 0xFFU);
    frame[1] = static_cast<std::uint8_t>(run_level_words >> 8U);
    frame[2] = 0x00;
    frame[3] = 0x38;
    frame[4] = 1;
    frame[6] = 2;
    for (std::size_t i = 0; i < logical.size(); ++i) {
        frame[8U + (i ^ 1U)] = logical[i];
    }
    return frame;
}

wuffs_base__status decode(
    wuffs_stuntmaster_psx__mdec_decoder* decoder,
    std::vector<std::uint8_t>& output,
    std::vector<std::uint8_t>& frame) {
    return wuffs_stuntmaster_psx__mdec_decoder__decode_frame(
        decoder,
        wuffs_base__make_slice_u8(output.data(), output.size()),
        wuffs_base__make_slice_u8(frame.data(), frame.size()));
}

void assertError(wuffs_base__status status, const char* expected) {
    assert(status.is_error());
    assert(status.message() != nullptr);
    assert(std::strcmp(status.message(), expected) == 0);
}

void constantNeutralFrame() {
    auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
    assert(decoder != nullptr);
    auto frame = constantFrame(0);
    std::vector<std::uint8_t> output(kRgbaBytes, 0xA5);
    const auto status = decode(decoder, output, frame);
    assert(status.is_ok());
    assert(wuffs_stuntmaster_psx__mdec_decoder__compressed_bytes_consumed(
               decoder) == kFrameBytes);
    for (std::size_t i = 0; i < output.size(); i += 4U) {
        assert(output[i + 0] == 128);
        assert(output[i + 1] == 128);
        assert(output[i + 2] == 128);
        assert(output[i + 3] == 255);
    }
    std::free(decoder);
}

void dcAndIdctScaling() {
    auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
    assert(decoder != nullptr);
    auto frame = constantFrame(80);
    std::vector<std::uint8_t> output(kRgbaBytes);
    const auto status = decode(decoder, output, frame);
    assert(status.is_ok());
    for (std::size_t i = 0; i < output.size(); i += 4U) {
        assert(output[i + 0] == 148);
        assert(output[i + 1] == 148);
        assert(output[i + 2] == 148);
        assert(output[i + 3] == 255);
    }
    std::free(decoder);
}

void escapeAndTableDequantization() {
    const std::uint8_t expected_escape[8] = {
        129, 128, 128, 128, 128, 128, 128, 127,
    };
    const std::uint8_t expected_table[8] = {
        129, 129, 128, 128, 128, 128, 127, 127,
    };
    for (const bool escape_coded : {false, true}) {
        auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
        assert(decoder != nullptr);
        auto frame = singleAcFrame(escape_coded);
        std::vector<std::uint8_t> output(kRgbaBytes);
        assert(decode(decoder, output, frame).is_ok());
        const auto* expected = escape_coded ? expected_escape : expected_table;
        for (std::size_t x = 0; x < 8U; ++x) {
            assert(output[x * 4U + 0U] == expected[x]);
            assert(output[x * 4U + 1U] == expected[x]);
            assert(output[x * 4U + 2U] == expected[x]);
            assert(output[x * 4U + 3U] == 255);
        }
        std::free(decoder);
    }
}

void exactIdctRounding() {
    const std::uint8_t expected[64] = {
        103, 103, 104, 105, 106, 106, 107, 107,
        103, 104, 104, 105, 106, 107, 107, 108,
        104, 104, 105, 106, 106, 107, 108, 108,
        105, 105, 106, 106, 107, 108, 109, 109,
        106, 106, 106, 107, 108, 109, 109, 110,
        106, 107, 107, 108, 109, 110, 110, 110,
        107, 107, 108, 109, 109, 110, 111, 111,
        107, 108, 108, 109, 110, 110, 111, 111,
    };
    auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
    assert(decoder != nullptr);
    auto frame = idctRoundingFrame();
    std::vector<std::uint8_t> output(kRgbaBytes);
    assert(decode(decoder, output, frame).is_ok());
    for (std::size_t y = 0; y < 8U; ++y) {
        for (std::size_t x = 0; x < 8U; ++x) {
            const auto rgba = ((y * 320U) + x) * 4U;
            const auto value = expected[y * 8U + x];
            assert(output[rgba + 0U] == value);
            assert(output[rgba + 1U] == value);
            assert(output[rgba + 2U] == value);
            assert(output[rgba + 3U] == 255);
        }
    }
    std::free(decoder);
}

void chromaBlockOrder() {
    auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
    assert(decoder != nullptr);
    // Retail BS v2 stores Cr first and Cb second. Distinct signed DC values
    // make a reversal visible even though neutral-chroma vectors cannot.
    auto frame = constantFrame(0, 80, -80);
    std::vector<std::uint8_t> output(kRgbaBytes);
    const auto status = decode(decoder, output, frame);
    assert(status.is_ok());
    for (std::size_t i = 0; i < output.size(); i += 4U) {
        assert(output[i + 0] == 156);
        assert(output[i + 1] == 121);
        assert(output[i + 2] == 93);
        assert(output[i + 3] == 255);
    }
    std::free(decoder);
}

void malformedInputsFailClosed() {
    std::vector<std::uint8_t> output(kRgbaBytes);
    auto frame = constantFrame(0);

    {
        auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
        assert(decoder != nullptr);
        output.pop_back();
        assertError(
            decode(decoder, output, frame),
            "stuntmaster_psx: wrong MDEC output size");
        output.push_back(0);
        std::free(decoder);
    }
    {
        auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
        assert(decoder != nullptr);
        auto wrong_version = frame;
        wrong_version[6] = 3;
        assertError(
            decode(decoder, output, wrong_version),
            "stuntmaster_psx: bad MDEC header");
        std::free(decoder);
    }
    {
        auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
        assert(decoder != nullptr);
        auto truncated = frame;
        truncated.pop_back();
        assertError(
            decode(decoder, output, truncated),
            "stuntmaster_psx: truncated MDEC input");
        std::free(decoder);
    }
    {
        auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
        assert(decoder != nullptr);
        auto invalid_vlc = frame;
        // First DC is zero. Clear the following EOB's leading one, leaving the
        // explicitly forbidden all-zero 12-bit Huffman prefix.
        invalid_vlc[8U + (1U ^ 1U)] &= static_cast<std::uint8_t>(~0x20U);
        assertError(
            decode(decoder, output, invalid_vlc),
            "stuntmaster_psx: bad MDEC Huffman code");
        std::free(decoder);
    }
    {
        auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
        assert(decoder != nullptr);
        auto wrong_rlsize = frame;
        wrong_rlsize[0] ^= 1U;
        assertError(
            decode(decoder, output, wrong_rlsize),
            "stuntmaster_psx: bad MDEC framing");
        std::free(decoder);
    }
    {
        auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
        assert(decoder != nullptr);
        auto missing_footer = frame;
        missing_footer.resize(missing_footer.size() - 4U);
        assertError(
            decode(decoder, output, missing_footer),
            "stuntmaster_psx: truncated MDEC input");
        std::free(decoder);
    }
    {
        auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
        assert(decoder != nullptr);
        auto corrupt_footer = frame;
        const auto marker_position = kBlocks * 12U;
        corrupt_footer[8U + ((marker_position / 8U) ^ 1U)] ^=
            static_cast<std::uint8_t>(0x80U >> (marker_position % 8U));
        assertError(
            decode(decoder, output, corrupt_footer),
            "stuntmaster_psx: bad MDEC framing");
        std::free(decoder);
    }
    {
        auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
        assert(decoder != nullptr);
        auto trailing_garbage = frame;
        trailing_garbage.insert(trailing_garbage.end(), 4U, 0xA5U);
        assertError(
            decode(decoder, output, trailing_garbage),
            "stuntmaster_psx: bad MDEC framing");
        std::free(decoder);
    }
    {
        auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
        assert(decoder != nullptr);
        auto nonzero_padding = frame;
        const auto padding_position = kPayloadBits;
        nonzero_padding[8U + ((padding_position / 8U) ^ 1U)] |=
            static_cast<std::uint8_t>(0x80U >> (padding_position % 8U));
        assertError(
            decode(decoder, output, nonzero_padding),
            "stuntmaster_psx: bad MDEC framing");
        std::free(decoder);
    }
}

std::uint16_t get16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
        (static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t get32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8U) |
        (static_cast<std::uint32_t>(data[2]) << 16U) |
        (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::vector<std::uint8_t> extractedFrame(
    const std::vector<std::uint8_t>& movie,
    std::uint32_t wanted_frame) {
    constexpr std::size_t kLogicalSector = 2048;
    constexpr std::size_t kHeader = 32;
    constexpr std::size_t kPayload = kLogicalSector - kHeader;
    std::vector<std::uint8_t> result;
    std::uint32_t expected_size = 0;
    std::uint16_t expected_chunks = 0;
    std::uint16_t next_chunk = 0;
    for (std::size_t offset = 0;
         offset + kLogicalSector <= movie.size(); offset += kLogicalSector) {
        const auto* sector = movie.data() + offset;
        if (get16(sector + 0) != 0x0160U ||
            get16(sector + 2) != 0x8001U ||
            get32(sector + 8) != wanted_frame) {
            continue;
        }
        const auto chunk = get16(sector + 4);
        const auto chunks = get16(sector + 6);
        const auto size = get32(sector + 12);
        if (chunk == 0) {
            expected_size = size;
            expected_chunks = chunks;
            next_chunk = 0;
            result.clear();
            result.reserve(size);
        }
        if (expected_size == 0 || chunks != expected_chunks ||
            size != expected_size || chunk != next_chunk) {
            throw std::runtime_error("inconsistent extracted STR frame");
        }
        const auto count = std::min<std::size_t>(
            kPayload, expected_size - result.size());
        result.insert(result.end(), sector + kHeader, sector + kHeader + count);
        ++next_chunk;
        if (result.size() == expected_size) {
            return result;
        }
    }
    throw std::runtime_error("requested STR frame was not found");
}

std::uint64_t fnv1a(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

void probeExtractedMovie(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open extracted STR");
    }
    const std::vector<std::uint8_t> movie{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    for (const auto frame_number : {1U, 61U, 122U}) {
        auto frame = extractedFrame(movie, frame_number);
        std::vector<std::uint8_t> output(kRgbaBytes);
        auto* decoder = wuffs_stuntmaster_psx__mdec_decoder__alloc();
        if (decoder == nullptr) {
            throw std::bad_alloc{};
        }
        const auto status = decode(decoder, output, frame);
        if (!status.is_ok()) {
            const auto* message = status.message();
            std::free(decoder);
            throw std::runtime_error(message == nullptr ? "MDEC error" : message);
        }
        std::cout << "frame=" << frame_number
                  << " encoded_bytes=" << frame.size()
                  << " consumed="
                  << wuffs_stuntmaster_psx__mdec_decoder__compressed_bytes_consumed(
                         decoder)
                  << " rgba_fnv1a=" << std::hex << std::setfill('0')
                  << std::setw(16) << fnv1a(output) << std::dec << '\n';
        std::free(decoder);
    }
}

} // namespace

int main(int argc, char** argv) {
    constantNeutralFrame();
    dcAndIdctScaling();
    escapeAndTableDequantization();
    exactIdctRounding();
    chromaBlockOrder();
    malformedInputsFailClosed();
    if (argc == 2) {
        try {
            probeExtractedMovie(argv[1]);
        } catch (const std::exception& error) {
            std::cerr << "MDEC disc probe: " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
