#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__STUNTMASTER_PSX
#include "stuntmaster_str.c"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>

namespace {

using Sector = std::array<std::uint8_t, 2352>;
using Demuxer = wuffs_stuntmaster_psx__str_demuxer;
using DemuxerPtr = std::unique_ptr<Demuxer, decltype(&std::free)>;

void put16(Sector& sector, std::size_t offset, std::uint16_t value) {
    sector[offset + 0] = static_cast<std::uint8_t>(value >> 0);
    sector[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(Sector& sector, std::size_t offset, std::uint32_t value) {
    sector[offset + 0] = static_cast<std::uint8_t>(value >> 0);
    sector[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    sector[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    sector[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

Sector makeMode2Sector(
    std::uint8_t channel,
    std::uint8_t submode,
    std::uint8_t coding_info) {
    Sector sector{};
    for (std::size_t i = 1; i < 11; ++i) {
        sector[i] = 0xFF;
    }
    sector[15] = 2;
    sector[16] = 1;
    sector[17] = channel;
    sector[18] = submode;
    sector[19] = coding_info;
    for (std::size_t i = 0; i < 4; ++i) {
        sector[20 + i] = sector[16 + i];
    }
    return sector;
}

Sector makeVideoSector(
    std::uint32_t frame,
    std::uint16_t chunk,
    std::uint16_t chunks,
    std::uint32_t encoded_size,
    std::uint16_t qscale = 3) {
    auto sector = makeMode2Sector(1, 0x48, 0);
    put32(sector, 24, 0x80010160U);
    put16(sector, 28, chunk);
    put16(sector, 30, chunks);
    put32(sector, 32, frame);
    put32(sector, 36, encoded_size);
    put16(sector, 40, 320);
    put16(sector, 42, 240);
    if (chunk == 0) {
        put16(sector, 56, 0x1234);
        put16(sector, 58, 0x3800);
        put16(sector, 60, qscale);
        put16(sector, 62, 2);
    }
    return sector;
}

wuffs_base__slice_u8 asSlice(Sector& sector) {
    return wuffs_base__make_slice_u8(sector.data(), sector.size());
}

bool isStatus(wuffs_base__status status, const char* expected) {
    return std::string_view{status.repr == nullptr ? "" : status.repr} == expected;
}

DemuxerPtr makeDemuxer() {
    return DemuxerPtr{
        wuffs_stuntmaster_psx__str_demuxer__alloc(), &std::free};
}

bool testValidInterleavingAndPadding() {
    auto demuxer = makeDemuxer();
    if (!demuxer) {
        return false;
    }

    constexpr std::uint32_t encoded_size = 2500;
    for (std::uint16_t chunk = 0; chunk < 6; ++chunk) {
        auto sector = makeVideoSector(1, chunk, 6, encoded_size);
        if (chunk == 5) {
            sector[18] = sector[22] = 0xC8;
        }
        if (!wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                 demuxer.get(), asSlice(sector))
                 .is_ok()) {
            return false;
        }
        const auto expected_length = chunk == 0 ? 2016U :
            (chunk == 1 ? 484U : 0U);
        if (wuffs_stuntmaster_psx__str_demuxer__sector_kind(demuxer.get()) !=
                WUFFS_STUNTMASTER_PSX__STR_SECTOR_KIND__VIDEO ||
            wuffs_stuntmaster_psx__str_demuxer__payload_offset(demuxer.get()) !=
                56U ||
            wuffs_stuntmaster_psx__str_demuxer__payload_length(demuxer.get()) !=
                expected_length ||
            wuffs_stuntmaster_psx__str_demuxer__frame_number(demuxer.get()) !=
                1U ||
            wuffs_stuntmaster_psx__str_demuxer__chunk_index(demuxer.get()) !=
                chunk ||
            wuffs_stuntmaster_psx__str_demuxer__chunk_count(demuxer.get()) !=
                6U ||
            wuffs_stuntmaster_psx__str_demuxer__encoded_frame_size(
                demuxer.get()) != encoded_size ||
            wuffs_stuntmaster_psx__str_demuxer__qscale(demuxer.get()) != 3U ||
            wuffs_stuntmaster_psx__str_demuxer__frame_complete(demuxer.get()) !=
                (chunk == 5 ? 1U : 0U)) {
            return false;
        }

        if (chunk == 2) {
            auto audio = makeMode2Sector(1, 0x64, 0x05);
            if (!wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                     demuxer.get(), asSlice(audio))
                     .is_ok() ||
                wuffs_stuntmaster_psx__str_demuxer__sector_kind(
                    demuxer.get()) !=
                    WUFFS_STUNTMASTER_PSX__STR_SECTOR_KIND__AUDIO ||
                wuffs_stuntmaster_psx__str_demuxer__payload_offset(
                    demuxer.get()) != 24U ||
                wuffs_stuntmaster_psx__str_demuxer__payload_length(
                    demuxer.get()) != 2304U ||
                wuffs_stuntmaster_psx__str_demuxer__audio_coding_info(
                    demuxer.get()) != 0x05U) {
                return false;
            }
        }
    }

    auto final_audio = makeMode2Sector(1, 0xE4, 0x01);
    auto empty = makeMode2Sector(0, 0, 0);
    return wuffs_stuntmaster_psx__str_demuxer__parse_sector(
               demuxer.get(), asSlice(final_audio))
               .is_ok() &&
        wuffs_stuntmaster_psx__str_demuxer__parse_sector(
            demuxer.get(), asSlice(empty))
            .is_ok() &&
        wuffs_stuntmaster_psx__str_demuxer__end_of_stream(demuxer.get())
            .is_ok() &&
        wuffs_stuntmaster_psx__str_demuxer__video_frame_count(demuxer.get()) ==
            1U &&
        wuffs_stuntmaster_psx__str_demuxer__audio_sector_count(demuxer.get()) ==
            2U;
}

bool testEnvelopeFailures() {
    auto demuxer = makeDemuxer();
    auto sector = makeVideoSector(1, 0, 6, 2500);
    if (!demuxer) {
        return false;
    }

    auto truncated = wuffs_base__make_slice_u8(sector.data(), sector.size() - 1);
    if (!isStatus(
            wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                demuxer.get(), truncated),
            "#stuntmaster_psx: truncated STR sector")) {
        return false;
    }

    std::array<std::uint8_t, 2353> too_long{};
    if (!isStatus(
            wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                demuxer.get(),
                wuffs_base__make_slice_u8(too_long.data(), too_long.size())),
            "#stuntmaster_psx: bad STR sector")) {
        return false;
    }

    sector[5] = 0;
    if (!isStatus(
            wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                demuxer.get(), asSlice(sector)),
            "#stuntmaster_psx: bad STR sector")) {
        return false;
    }
    sector = makeVideoSector(1, 0, 6, 2500);
    sector[21] ^= 1;
    return isStatus(
        wuffs_stuntmaster_psx__str_demuxer__parse_sector(
            demuxer.get(), asSlice(sector)),
        "#stuntmaster_psx: bad STR sector");
}

bool testUnsupportedFields() {
    const auto rejected = [](Sector sector) {
        auto demuxer = makeDemuxer();
        return demuxer && isStatus(
            wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                demuxer.get(), asSlice(sector)),
            "#stuntmaster_psx: unsupported STR sector");
    };

    auto wrong_channel = makeVideoSector(1, 0, 6, 2500);
    wrong_channel[17] = wrong_channel[21] = 2;
    auto wrong_dimensions = makeVideoSector(1, 0, 6, 2500);
    put16(wrong_dimensions, 40, 321);
    auto too_few_chunks = makeVideoSector(1, 0, 5, 2500);
    auto too_many_chunks = makeVideoSector(1, 0, 12, 2500);
    auto chunk_out_of_range = makeVideoSector(1, 6, 6, 2500);
    auto empty_frame = makeVideoSector(1, 0, 6, 0);
    auto oversized = makeVideoSector(1, 0, 11, 22177);
    auto wrapped_frame = makeVideoSector(0xFFFFFFFFU, 0, 6, 2500);
    auto wrong_version = makeVideoSector(1, 0, 6, 2500);
    put16(wrong_version, 62, 3);
    auto zero_qscale = makeVideoSector(1, 0, 6, 2500, 0);
    auto high_qscale = makeVideoSector(1, 0, 6, 2500, 8);
    auto wrong_audio = makeMode2Sector(1, 0x64, 0x00);
    return rejected(wrong_channel) && rejected(wrong_dimensions) &&
        rejected(too_few_chunks) && rejected(too_many_chunks) &&
        rejected(chunk_out_of_range) && rejected(empty_frame) &&
        rejected(oversized) && rejected(wrapped_frame) &&
        rejected(wrong_version) && rejected(zero_qscale) &&
        rejected(high_qscale) && rejected(wrong_audio);
}

bool testAcceptedCountAndSizeBoundaries() {
    auto demuxer = makeDemuxer();
    auto maximal = makeVideoSector(1, 0, 11, 22176, 7);
    if (!demuxer ||
        !wuffs_stuntmaster_psx__str_demuxer__parse_sector(
             demuxer.get(), asSlice(maximal))
             .is_ok() ||
        wuffs_stuntmaster_psx__str_demuxer__payload_length(demuxer.get()) !=
            2016U ||
        wuffs_stuntmaster_psx__str_demuxer__qscale(demuxer.get()) != 7U) {
        return false;
    }
    wuffs_stuntmaster_psx__str_demuxer__restart(demuxer.get());
    auto minimal = makeVideoSector(1, 0, 6, 1, 1);
    return wuffs_stuntmaster_psx__str_demuxer__parse_sector(
               demuxer.get(), asSlice(minimal))
               .is_ok() &&
        wuffs_stuntmaster_psx__str_demuxer__payload_length(demuxer.get()) ==
            1U &&
        wuffs_stuntmaster_psx__str_demuxer__qscale(demuxer.get()) == 1U;
}

bool testSequenceFailures() {
    {
        auto demuxer = makeDemuxer();
        auto first = makeVideoSector(1, 0, 6, 2500);
        auto duplicate = makeVideoSector(1, 0, 6, 2500);
        if (!demuxer ||
            !wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                 demuxer.get(), asSlice(first))
                 .is_ok() ||
            !isStatus(
                wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                    demuxer.get(), asSlice(duplicate)),
                "#stuntmaster_psx: bad STR sequence")) {
            return false;
        }
    }
    {
        auto demuxer = makeDemuxer();
        auto first = makeVideoSector(1, 0, 6, 2500);
        auto reordered = makeVideoSector(1, 2, 6, 2500);
        if (!demuxer ||
            !wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                 demuxer.get(), asSlice(first))
                 .is_ok() ||
            !isStatus(
                wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                    demuxer.get(), asSlice(reordered)),
                "#stuntmaster_psx: bad STR sequence") ||
            !isStatus(
                wuffs_stuntmaster_psx__str_demuxer__end_of_stream(
                    demuxer.get()),
                "#stuntmaster_psx: bad STR sequence")) {
            return false;
        }
    }
    {
        auto demuxer = makeDemuxer();
        auto wrong_first_frame = makeVideoSector(2, 0, 6, 2500);
        if (!demuxer || !isStatus(
                wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                    demuxer.get(), asSlice(wrong_first_frame)),
                "#stuntmaster_psx: bad STR sequence")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    return testValidInterleavingAndPadding() && testEnvelopeFailures() &&
            testUnsupportedFields() && testAcceptedCountAndSizeBoundaries() &&
            testSequenceFailures()
        ? 0
        : 1;
}
