// Playback structure adapted from SF-pc-port d9522cd under the MIT License.
#include "stuntmaster/presentation/movie_player.hpp"

#include "stuntmaster/core/error.hpp"
#include "stuntmaster/media/str_decoder.hpp"
#include "stuntmaster/presentation/psycross_presenter.hpp"

#include <AL/al.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <variant>
#include <vector>

namespace stuntmaster::presentation {
namespace {

class MovieAudioPlayer final {
public:
    explicit MovieAudioPlayer(bool enabled) : enabled_(enabled) {
        if (!enabled_) {
            return;
        }
        alGetError();
        alGenSources(1, &source_);
        if (alGetError() != AL_NO_ERROR || source_ == 0U) {
            enabled_ = false;
            source_ = 0U;
            std::cerr << "movie audio: unavailable; continuing silently\n";
            return;
        }
        buffers_.reserve(maximum_queued_buffers);
        available_.reserve(maximum_queued_buffers);
        staged_samples_.reserve(
            maximum_queued_buffers * sample_frames_per_buffer * 2U);
    }

    ~MovieAudioPlayer() {
        if (source_ == 0U) {
            return;
        }
        alSourceStop(source_);
        ALint queued{};
        alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
        while (queued-- > 0) {
            ALuint buffer{};
            alSourceUnqueueBuffers(source_, 1, &buffer);
        }
        alDeleteSources(1, &source_);
        if (!buffers_.empty()) {
            alDeleteBuffers(
                static_cast<ALsizei>(buffers_.size()), buffers_.data());
        }
        alGetError();
    }

    MovieAudioPlayer(const MovieAudioPlayer&) = delete;
    MovieAudioPlayer& operator=(const MovieAudioPlayer&) = delete;

    void queue(const media::MovieAudioChunk& chunk) {
        if (!enabled_ || chunk.stereo_samples.empty()) {
            return;
        }
        if (chunk.sample_rate <= 0 ||
            (chunk.stereo_samples.size() % 2U) != 0U) {
            throw core::Error{"Invalid movie audio chunk"};
        }
        if (sample_rate_ == 0) {
            sample_rate_ = chunk.sample_rate;
        } else if (sample_rate_ != chunk.sample_rate) {
            throw core::Error{"STR audio sample rate changed during playback"};
        }
        compactStaging();
        staged_samples_.insert(
            staged_samples_.end(), chunk.stereo_samples.begin(),
            chunk.stereo_samples.end());
        update();
    }

    void start() {
        start_requested_ = true;
        startIfNeeded();
    }

    void finishInput() {
        input_finished_ = true;
        collectProcessed();
        uploadReadyBuffers(true);
        startIfNeeded();
    }

    void update() {
        if (!enabled_) {
            return;
        }
        collectProcessed();
        uploadReadyBuffers(input_finished_);
        startIfNeeded();
    }

    [[nodiscard]] bool empty() const {
        if (!enabled_) {
            return true;
        }
        ALint queued{};
        alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
        return queued == 0 && staged_offset_ == staged_samples_.size();
    }

private:
    static constexpr std::size_t sample_frames_per_buffer = 1024U;
    static constexpr std::size_t maximum_queued_buffers = 16U;

    void uploadReadyBuffers(bool flush_partial) {
        while (queuedBufferCount() < maximum_queued_buffers) {
            const auto remaining_frames =
                (staged_samples_.size() - staged_offset_) / 2U;
            if (remaining_frames < sample_frames_per_buffer &&
                (!flush_partial || remaining_frames == 0U)) {
                break;
            }
            const auto frame_count =
                std::min(remaining_frames, sample_frames_per_buffer);
            uploadBuffer(std::span<const std::int16_t>{staged_samples_}.subspan(
                staged_offset_, frame_count * 2U));
            staged_offset_ += frame_count * 2U;
        }
        compactStaging();
    }

    void uploadBuffer(std::span<const std::int16_t> samples) {
        ALuint buffer{};
        if (available_.empty()) {
            if (buffers_.size() >= maximum_queued_buffers) {
                return;
            }
            alGenBuffers(1, &buffer);
            if (alGetError() != AL_NO_ERROR || buffer == 0U) {
                throw core::Error{"Cannot create movie audio buffer"};
            }
            buffers_.push_back(buffer);
        } else {
            buffer = available_.back();
            available_.pop_back();
        }
        if (samples.size() >
            static_cast<std::size_t>(std::numeric_limits<ALsizei>::max()) /
                sizeof(std::int16_t)) {
            throw core::Error{"Movie audio buffer is too large"};
        }
        alBufferData(
            buffer, AL_FORMAT_STEREO16, samples.data(),
            static_cast<ALsizei>(samples.size() * sizeof(std::int16_t)),
            sample_rate_);
        alSourceQueueBuffers(source_, 1, &buffer);
        if (alGetError() != AL_NO_ERROR) {
            throw core::Error{"Cannot queue movie audio"};
        }
    }

    void compactStaging() {
        if (staged_offset_ == 0U) {
            return;
        }
        if (staged_offset_ == staged_samples_.size()) {
            staged_samples_.clear();
            staged_offset_ = 0U;
        } else if (
            staged_offset_ >= sample_frames_per_buffer * 2U &&
            staged_offset_ * 2U >= staged_samples_.size()) {
            staged_samples_.erase(
                staged_samples_.begin(),
                staged_samples_.begin() +
                    static_cast<std::ptrdiff_t>(staged_offset_));
            staged_offset_ = 0U;
        }
    }

    [[nodiscard]] std::size_t queuedBufferCount() const {
        ALint queued{};
        alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
        return queued > 0 ? static_cast<std::size_t>(queued) : 0U;
    }

    void collectProcessed() {
        ALint processed{};
        alGetSourcei(source_, AL_BUFFERS_PROCESSED, &processed);
        while (processed-- > 0) {
            ALuint buffer{};
            alSourceUnqueueBuffers(source_, 1, &buffer);
            available_.push_back(buffer);
        }
    }

    void startIfNeeded() {
        if (!enabled_ || !start_requested_) {
            return;
        }
        ALint queued{};
        ALint state{};
        alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
        alGetSourcei(source_, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING &&
            (queued >= 3 || (input_finished_ && queued > 0))) {
            alSourcePlay(source_);
        }
    }

    bool enabled_{};
    ALuint source_{};
    std::vector<ALuint> buffers_;
    std::vector<ALuint> available_;
    std::vector<std::int16_t> staged_samples_;
    std::size_t staged_offset_{};
    int sample_rate_{};
    bool start_requested_{};
    bool input_finished_{};
};

class BufferedMovieStream final {
public:
    explicit BufferedMovieStream(std::vector<std::byte> sectors)
        : decoder_(media::StrDecoder::open(std::move(sectors))) {}

    [[nodiscard]] double framesPerSecond() const noexcept {
        return decoder_.framesPerSecond();
    }

    void fill(std::size_t target_video_frames, MovieAudioPlayer& audio) {
        while (!finished_ && video_frames_.size() < target_video_frames) {
            auto event = decoder_.next();
            if (!event) {
                finished_ = true;
                audio.finishInput();
                break;
            }
            if (auto* frame = std::get_if<media::MovieVideoFrame>(&*event)) {
                video_frames_.emplace_back(std::move(*frame));
            } else {
                audio.queue(std::get<media::MovieAudioChunk>(*event));
            }
        }
    }

    [[nodiscard]] bool hasVideoFrame() const noexcept {
        return !video_frames_.empty();
    }

    [[nodiscard]] std::optional<double> nextVideoTimestamp() const noexcept {
        if (video_frames_.empty()) {
            return std::nullopt;
        }
        return video_frames_.front().timestamp_seconds;
    }

    [[nodiscard]] media::MovieVideoFrame takeVideoFrame() {
        auto result = std::move(video_frames_.front());
        video_frames_.pop_front();
        return result;
    }

private:
    media::StrDecoder decoder_;
    std::deque<media::MovieVideoFrame> video_frames_;
    bool finished_{};
};

class MovieFrameTiming final {
public:
    explicit MovieFrameTiming(double frames_per_second)
        : frames_per_second_(frames_per_second) {}

    [[nodiscard]] bool valid() const noexcept {
        return std::isfinite(frames_per_second_) &&
            frames_per_second_ > 0.0 && frames_per_second_ <= 120.0;
    }

    [[nodiscard]] double frameEndSeconds(
        double frame_timestamp,
        std::optional<double> next_timestamp) {
        const auto frame_step = 1.0 / frames_per_second_;
        const auto fallback = last_deadline_ + frame_step;
        const auto timestamp_valid = [](double value) {
            return std::isfinite(value) && value >= 0.0;
        };
        if (!timestamp_origin_ && timestamp_valid(frame_timestamp)) {
            timestamp_origin_ = frame_timestamp;
        }
        auto deadline = fallback;
        if (timestamp_origin_ && next_timestamp &&
            timestamp_valid(*next_timestamp)) {
            const auto candidate = *next_timestamp - *timestamp_origin_;
            const auto step = candidate - last_deadline_;
            if (std::isfinite(candidate) && step > 0.0 &&
                step <= frame_step * 4.0) {
                deadline = candidate;
            } else {
                timestamp_origin_ = *next_timestamp - fallback;
            }
        }
        last_deadline_ = deadline;
        return deadline;
    }

private:
    double frames_per_second_{};
    std::optional<double> timestamp_origin_;
    double last_deadline_{};
};

} // namespace

MoviePlaybackResult playMovie(
    PsyCrossPresenter& presenter,
    std::string_view path,
    std::vector<std::byte> raw_sectors,
    bool audio_enabled,
    MovieGuestAudioAdvance advance_guest_audio) {
    std::cout << "movie: playing " << path << '\n';
    const auto guest_audio_started = std::chrono::steady_clock::now();
    const auto serviceGuestAudio = [&] {
        if (advance_guest_audio) {
            advance_guest_audio(
                std::chrono::steady_clock::now() - guest_audio_started);
        }
    };
    MovieAudioPlayer audio{audio_enabled};
    BufferedMovieStream stream{std::move(raw_sectors)};
    MovieFrameTiming timing{stream.framesPerSecond()};
    if (!timing.valid()) {
        throw core::Error{"STR stream has an invalid video frame rate"};
    }

    constexpr std::size_t decoded_ahead_frames = 5U;
    stream.fill(decoded_ahead_frames, audio);
    serviceGuestAudio();
    auto previous_buttons = presenter.pollPadOneButtons();
    const auto started = std::chrono::steady_clock::now();
    audio.start();
    bool has_frame = false;

    while (stream.hasVideoFrame()) {
        auto frame = stream.takeVideoFrame();
        stream.fill(decoded_ahead_frames, audio);
        presenter.presentMovieFrame(
            frame.rgba8888,
            static_cast<std::uint32_t>(frame.width),
            static_cast<std::uint32_t>(frame.height));
        has_frame = true;
        const auto deadline = timing.frameEndSeconds(
            frame.timestamp_seconds, stream.nextVideoTimestamp());
        for (;;) {
            serviceGuestAudio();
            const auto buttons = presenter.pollPadOneButtons();
            if (movieStartPressed(previous_buttons, buttons)) {
                std::cout << "movie: skipped " << path << '\n';
                return MoviePlaybackResult::skipped;
            }
            previous_buttons = buttons;
            audio.update();
            const auto elapsed = std::chrono::duration<double>{
                std::chrono::steady_clock::now() - started}.count();
            if (elapsed >= deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    // XA can extend slightly beyond the final MDEC frame. Keep the last image
    // visible while draining, but bound malformed streams to two seconds.
    audio.finishInput();
    const auto drain_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (has_frame && !audio.empty() &&
           std::chrono::steady_clock::now() < drain_deadline) {
        serviceGuestAudio();
        const auto buttons = presenter.pollPadOneButtons();
        if (movieStartPressed(previous_buttons, buttons)) {
            std::cout << "movie: skipped " << path << '\n';
            return MoviePlaybackResult::skipped;
        }
        previous_buttons = buttons;
        audio.update();
        presenter.repeatScanout();
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    }
    serviceGuestAudio();
    std::cout << "movie: completed " << path << '\n';
    return MoviePlaybackResult::completed;
}

} // namespace stuntmaster::presentation
