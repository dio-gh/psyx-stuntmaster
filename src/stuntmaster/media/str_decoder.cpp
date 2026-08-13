// SPDX-License-Identifier: MIT
#include "stuntmaster/media/str_decoder.hpp"

#include "stuntmaster/core/error.hpp"

#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__STUNTMASTER_PSX
#include "stuntmaster/media/wuffs/stuntmaster_codecs.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace stuntmaster::media {
namespace {

constexpr std::size_t raw_sector_size =
    WUFFS_STUNTMASTER_PSX__STR_RAW_SECTOR_LENGTH;
constexpr std::size_t xa_output_bytes =
    WUFFS_STUNTMASTER_PSX__XA_OUTPUT_BYTES;
constexpr std::size_t mdec_rgba_size =
    WUFFS_STUNTMASTER_PSX__MDEC_RGBA_SIZE;

template <typename T>
using WuffsPtr = std::unique_ptr<T, decltype(&std::free)>;

[[noreturn]] void throwStatus(
    const char* operation,
    wuffs_base__status status) {
    throw core::Error{
        std::string{operation} + ": " +
        (status.repr == nullptr ? "unknown Wuffs error" : status.repr)};
}

void requireWuffs(wuffs_base__status status, const char* operation) {
    if (!status.is_ok()) {
        throwStatus(operation, status);
    }
}

wuffs_base__slice_u8 writableSlice(std::span<std::byte> bytes) {
    return wuffs_base__make_slice_u8(
        reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size());
}

wuffs_base__slice_u8 writableSlice(std::span<std::uint8_t> bytes) {
    return wuffs_base__make_slice_u8(bytes.data(), bytes.size());
}

template <typename T>
WuffsPtr<T> requireAllocation(T* pointer, const char* description) {
    if (pointer == nullptr) {
        throw core::Error{std::string{"Cannot allocate "} + description};
    }
    return WuffsPtr<T>{pointer, &std::free};
}

} // namespace

struct StrDecoder::Impl {
    explicit Impl(std::vector<std::byte> source)
        : source(std::move(source)),
          demuxer(requireAllocation(
              wuffs_stuntmaster_psx__str_demuxer__alloc(),
              "Wuffs STR demuxer")),
          xa_decoder(requireAllocation(
              wuffs_stuntmaster_psx__xa_decoder__alloc(),
              "Wuffs XA decoder")),
          mdec_decoder(requireAllocation(
              wuffs_stuntmaster_psx__mdec_decoder__alloc(),
              "Wuffs MDEC decoder")) {}

    void initialize() {
        if (source.empty()) {
            throw core::Error{"STR source is empty"};
        }
        if ((source.size() % raw_sector_size) != 0U) {
            throw core::Error{"STR source is not a whole number of raw sectors"};
        }

        std::uint32_t detected_sample_rate = 0;
        for (std::size_t offset = 0; offset < source.size();
             offset += raw_sector_size) {
            auto sector = std::span{source}.subspan(offset, raw_sector_size);
            requireWuffs(
                wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                    demuxer.get(), writableSlice(sector)),
                "Cannot inspect STR sector");
            if (wuffs_stuntmaster_psx__str_demuxer__sector_kind(
                    demuxer.get()) ==
                WUFFS_STUNTMASTER_PSX__STR_SECTOR_KIND__AUDIO) {
                const auto coding_info = static_cast<std::uint8_t>(
                    wuffs_stuntmaster_psx__str_demuxer__audio_coding_info(
                        demuxer.get()));
                const auto rate =
                    wuffs_stuntmaster_psx__xa_decoder__sample_rate(
                        xa_decoder.get(), coding_info);
                if (rate == 0U ||
                    (detected_sample_rate != 0U &&
                     detected_sample_rate != rate)) {
                    throw core::Error{"STR audio format changes within stream"};
                }
                detected_sample_rate = rate;
            }
        }
        requireWuffs(
            wuffs_stuntmaster_psx__str_demuxer__end_of_stream(demuxer.get()),
            "Cannot finish STR inspection");

        video_frame_count =
            wuffs_stuntmaster_psx__str_demuxer__video_frame_count(
                demuxer.get());
        audio_sector_count =
            wuffs_stuntmaster_psx__str_demuxer__audio_sector_count(
                demuxer.get());
        sample_rate = static_cast<int>(detected_sample_rate);
        if (video_frame_count == 0U) {
            throw core::Error{"STR stream has no supported video"};
        }
        if (audio_sector_count != 0U && sample_rate != 0) {
            // Reproduce the retired psxstr demuxer's exact rational time
            // bases and floating-point evaluation order. Although these are
            // algebraically equivalent to deriving time from PCM frames,
            // reassociation changes the observable double values by one ULP.
            audio_time_base = sample_rate == 37800
                ? (4.0 / 75.0)
                : (8.0 / 75.0);
            const auto audio_seconds =
                static_cast<double>(audio_sector_count) * audio_time_base;
            frames_per_second = static_cast<double>(video_frame_count) /
                audio_seconds;
            video_timestamp_scale = 15.0 / frames_per_second;
        }
        wuffs_stuntmaster_psx__str_demuxer__restart(demuxer.get());
        wuffs_stuntmaster_psx__xa_decoder__reset_history(xa_decoder.get());
    }

    MovieVideoFrame decodeVideo() {
        MovieVideoFrame output;
        output.width = WUFFS_STUNTMASTER_PSX__MDEC_WIDTH;
        output.height = WUFFS_STUNTMASTER_PSX__MDEC_HEIGHT;
        output.timestamp_seconds =
            (static_cast<double>(emitted_video_frames) * (1.0 / 15.0)) *
            video_timestamp_scale;
        output.rgba8888.resize(mdec_rgba_size);
        requireWuffs(
            wuffs_stuntmaster_psx__mdec_decoder__decode_frame(
                mdec_decoder.get(),
                writableSlice(std::span{output.rgba8888}),
                writableSlice(std::span{encoded_video_frame})),
            "Cannot decode STR video frame");
        ++emitted_video_frames;
        encoded_video_frame.clear();
        return output;
    }

    MovieAudioChunk decodeAudio(
        std::span<std::byte> sector,
        std::uint32_t payload_offset,
        std::uint32_t payload_length,
        std::uint8_t coding_info) {
        if (payload_offset > sector.size() ||
            payload_length > sector.size() - payload_offset) {
            throw core::Error{"STR audio payload is out of bounds"};
        }
        MovieAudioChunk output;
        output.sample_rate = static_cast<int>(
            wuffs_stuntmaster_psx__xa_decoder__sample_rate(
                xa_decoder.get(), coding_info));
        output.timestamp_seconds =
            static_cast<double>(emitted_audio_sectors) * audio_time_base;
        output.stereo_samples.resize(xa_output_bytes / sizeof(std::int16_t));
        auto output_bytes = std::as_writable_bytes(
            std::span{output.stereo_samples});
        requireWuffs(
            wuffs_stuntmaster_psx__xa_decoder__decode_sector(
                xa_decoder.get(),
                writableSlice(output_bytes),
                writableSlice(sector.subspan(payload_offset, payload_length)),
                coding_info),
            "Cannot decode STR audio sector");
        ++emitted_audio_sectors;
        return output;
    }

    std::optional<MovieEvent> next() {
        while (cursor < source.size()) {
            auto sector =
                std::span{source}.subspan(cursor, raw_sector_size);
            cursor += raw_sector_size;
            requireWuffs(
                wuffs_stuntmaster_psx__str_demuxer__parse_sector(
                    demuxer.get(), writableSlice(sector)),
                "Cannot demux STR sector");

            const auto kind =
                wuffs_stuntmaster_psx__str_demuxer__sector_kind(
                    demuxer.get());
            const auto payload_offset =
                wuffs_stuntmaster_psx__str_demuxer__payload_offset(
                    demuxer.get());
            const auto payload_length =
                wuffs_stuntmaster_psx__str_demuxer__payload_length(
                    demuxer.get());
            if (payload_offset > sector.size() ||
                payload_length > sector.size() - payload_offset) {
                throw core::Error{"STR payload is out of bounds"};
            }

            if (kind == WUFFS_STUNTMASTER_PSX__STR_SECTOR_KIND__VIDEO) {
                if (wuffs_stuntmaster_psx__str_demuxer__chunk_index(
                        demuxer.get()) == 0U) {
                    encoded_video_frame.clear();
                    encoded_video_frame.reserve(
                        wuffs_stuntmaster_psx__str_demuxer__encoded_frame_size(
                            demuxer.get()));
                }
                const auto payload =
                    sector.subspan(payload_offset, payload_length);
                encoded_video_frame.insert(
                    encoded_video_frame.end(), payload.begin(), payload.end());
                if (wuffs_stuntmaster_psx__str_demuxer__frame_complete(
                        demuxer.get()) != 0U) {
                    const auto expected_size =
                        wuffs_stuntmaster_psx__str_demuxer__encoded_frame_size(
                            demuxer.get());
                    if (encoded_video_frame.size() != expected_size) {
                        throw core::Error{"STR video frame size is inconsistent"};
                    }
                    return MovieEvent{decodeVideo()};
                }
            } else if (
                kind == WUFFS_STUNTMASTER_PSX__STR_SECTOR_KIND__AUDIO) {
                return MovieEvent{decodeAudio(
                    sector,
                    payload_offset,
                    payload_length,
                    static_cast<std::uint8_t>(
                        wuffs_stuntmaster_psx__str_demuxer__audio_coding_info(
                            demuxer.get())))};
            }
        }
        if (!finished) {
            requireWuffs(
                wuffs_stuntmaster_psx__str_demuxer__end_of_stream(
                    demuxer.get()),
                "Cannot finish STR stream");
            finished = true;
        }
        return std::nullopt;
    }

    std::vector<std::byte> source;
    WuffsPtr<wuffs_stuntmaster_psx__str_demuxer> demuxer;
    WuffsPtr<wuffs_stuntmaster_psx__xa_decoder> xa_decoder;
    WuffsPtr<wuffs_stuntmaster_psx__mdec_decoder> mdec_decoder;
    std::vector<std::byte> encoded_video_frame;
    std::size_t cursor{};
    std::uint64_t video_frame_count{};
    std::uint64_t audio_sector_count{};
    std::uint64_t emitted_video_frames{};
    std::uint64_t emitted_audio_sectors{};
    int sample_rate{};
    double frames_per_second{15.0};
    double video_timestamp_scale{1.0};
    double audio_time_base{};
    bool finished{};
};

StrDecoder::StrDecoder(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

StrDecoder StrDecoder::open(std::vector<std::byte> raw_sectors) {
    auto impl = std::make_unique<Impl>(std::move(raw_sectors));
    impl->initialize();
    return StrDecoder{std::move(impl)};
}

StrDecoder::~StrDecoder() = default;
StrDecoder::StrDecoder(StrDecoder&&) noexcept = default;
StrDecoder& StrDecoder::operator=(StrDecoder&&) noexcept = default;

std::optional<MovieEvent> StrDecoder::next() {
    return impl_->next();
}

double StrDecoder::framesPerSecond() const noexcept {
    return impl_->frames_per_second;
}

bool StrDecoder::hasAudio() const noexcept {
    return impl_->audio_sector_count != 0U;
}

} // namespace stuntmaster::media
