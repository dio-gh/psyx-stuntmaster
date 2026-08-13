#include "stuntmaster/core/sha256.hpp"
#include "stuntmaster/disc/iso9660.hpp"
#include "stuntmaster/media/str_decoder.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::array movie_names{
    "FE/MOVIES/CREDITS.STR",
    "FE/MOVIES/DEMO.STR",
    "FE/MOVIES/DOLBY.STR",
    "FE/MOVIES/FACTORY.STR",
    "FE/MOVIES/MAKING.STR",
    "FE/MOVIES/MDWY320M.STR",
    "FE/MOVIES/PROLOG.STR",
    "FE/MOVIES/RADI.STR",
    "FE/MOVIES/VICTORY.STR",
};

void appendDigest(
    std::vector<std::byte>& aggregate,
    std::span<const std::byte> bytes) {
    const auto digest = stuntmaster::core::sha256(bytes);
    aggregate.insert(aggregate.end(), digest.begin(), digest.end());
}

void mixU64(std::uint64_t& hash, std::uint64_t value) {
    for (unsigned int index = 0; index < 8U; ++index) {
        hash ^= static_cast<std::uint8_t>(value >> (index * 8U));
        hash *= 1099511628211ULL;
    }
}

void appendU64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned int index = 0; index < 8U; ++index) {
        bytes.push_back(
            static_cast<std::byte>(static_cast<std::uint8_t>(
                value >> (index * 8U))));
    }
}

template <typename T>
std::span<const std::byte> asBytes(const std::vector<T>& values) {
    return std::as_bytes(std::span{values});
}

int probeMovie(
    stuntmaster::disc::Iso9660Image& image,
    const std::string& movie_name) {
    auto raw = image.readRawSectorFile(movie_name);
    auto decoder = stuntmaster::media::StrDecoder::open(std::move(raw.bytes));

    std::uint64_t video_frames = 0;
    std::uint64_t audio_chunks = 0;
    std::uint64_t audio_frames = 0;
    std::vector<std::byte> video_digests;
    std::vector<std::byte> audio_digests;
    std::vector<std::byte> event_stream;
    int width = 0;
    int height = 0;
    int sample_rate = 0;
    double last_video_timestamp = 0.0;
    double last_audio_timestamp = 0.0;
    std::uint64_t video_timestamp_hash = 14695981039346656037ULL;
    std::uint64_t audio_timestamp_hash = 14695981039346656037ULL;

    while (const auto event = decoder.next()) {
        if (const auto* video =
                std::get_if<stuntmaster::media::MovieVideoFrame>(&*event)) {
            ++video_frames;
            width = video->width;
            height = video->height;
            last_video_timestamp = video->timestamp_seconds;
            mixU64(
                video_timestamp_hash,
                std::bit_cast<std::uint64_t>(video->timestamp_seconds));
            appendDigest(video_digests, asBytes(video->rgba8888));
            event_stream.push_back(std::byte{0x01});
            appendU64(event_stream, static_cast<std::uint64_t>(video->width));
            appendU64(event_stream, static_cast<std::uint64_t>(video->height));
            appendU64(
                event_stream,
                std::bit_cast<std::uint64_t>(video->timestamp_seconds));
            appendU64(event_stream, video->rgba8888.size());
            appendDigest(event_stream, asBytes(video->rgba8888));
        } else {
            const auto& audio =
                std::get<stuntmaster::media::MovieAudioChunk>(*event);
            ++audio_chunks;
            audio_frames += audio.stereo_samples.size() / 2U;
            sample_rate = audio.sample_rate;
            last_audio_timestamp = audio.timestamp_seconds;
            mixU64(
                audio_timestamp_hash,
                std::bit_cast<std::uint64_t>(audio.timestamp_seconds));
            appendDigest(audio_digests, asBytes(audio.stereo_samples));
            event_stream.push_back(std::byte{0x02});
            appendU64(
                event_stream,
                static_cast<std::uint64_t>(audio.sample_rate));
            appendU64(
                event_stream,
                std::bit_cast<std::uint64_t>(audio.timestamp_seconds));
            appendU64(event_stream, audio.stereo_samples.size());
            appendDigest(event_stream, asBytes(audio.stereo_samples));
        }
    }

    const auto video_digest = stuntmaster::core::toHex(
        stuntmaster::core::sha256(video_digests));
    const auto audio_digest = stuntmaster::core::toHex(
        stuntmaster::core::sha256(audio_digests));
    const auto event_digest = stuntmaster::core::toHex(
        stuntmaster::core::sha256(event_stream));
    std::cout << movie_name
              << " sectors=" << raw.sector_count
              << " fps=" << std::fixed << std::setprecision(9)
              << decoder.framesPerSecond()
              << " fps_bits=" << std::hex << std::setfill('0')
              << std::setw(16)
              << std::bit_cast<std::uint64_t>(decoder.framesPerSecond())
              << " has_audio=" << std::dec << (decoder.hasAudio() ? 1 : 0)
              << std::hex
              << " video_ts_fnv1a=" << std::setw(16)
              << video_timestamp_hash
              << " audio_ts_fnv1a=" << std::setw(16)
              << audio_timestamp_hash << std::dec << std::setfill(' ')
              << " video_frames=" << video_frames
              << " dimensions=" << width << 'x' << height
              << " video_sha256=" << video_digest
              << " last_video_ts=" << last_video_timestamp
              << " audio_chunks=" << audio_chunks
              << " audio_frames=" << audio_frames
              << " sample_rate=" << sample_rate
              << " audio_sha256=" << audio_digest
              << " event_sha256=" << event_digest
              << " last_audio_ts=" << last_audio_timestamp
              << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: stuntmaster_str_codec_probe <game.cue> [MOVIE.STR ...]\n";
        return 2;
    }
    try {
        auto image = stuntmaster::disc::Iso9660Image::open(
            std::filesystem::path{argv[1]});
        if (argc > 2) {
            for (int index = 2; index < argc; ++index) {
                probeMovie(image, argv[index]);
            }
        } else {
            for (const auto* movie_name : movie_names) {
                probeMovie(image, movie_name);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "codec probe: " << error.what() << '\n';
        return 1;
    }
}
