#include "stuntmaster/presentation/audio_output.hpp"

#include <AL/al.h>
#include <AL/alc.h>

#include <array>
#include <iostream>

namespace stuntmaster::presentation {
namespace {

// Four buffers of roughly a sixtieth of a second each. The guest produces 735
// stereo frames per VBlank, so this is about four frames of slack: enough to
// ride out a late presentation without adding latency anyone would notice.
constexpr std::size_t buffer_count = 4U;

} // namespace

AudioOutput::AudioOutput() {
    auto* device = alcOpenDevice(nullptr);
    if (device == nullptr) {
        std::cerr << "audio: no OpenAL device; continuing without sound\n";
        return;
    }
    auto* context = alcCreateContext(device, nullptr);
    if (context == nullptr || alcMakeContextCurrent(context) == ALC_FALSE) {
        if (context != nullptr) {
            alcDestroyContext(context);
        }
        alcCloseDevice(device);
        std::cerr << "audio: no OpenAL context; continuing without sound\n";
        return;
    }
    device_ = device;
    context_ = context;

    ALuint source{};
    alGenSources(1, &source);
    source_ = source;
    std::array<ALuint, buffer_count> buffers{};
    alGenBuffers(static_cast<ALsizei>(buffers.size()), buffers.data());
    free_buffers_.assign(buffers.begin(), buffers.end());
    ready_ = alGetError() == AL_NO_ERROR;
    if (!ready_) {
        std::cerr << "audio: OpenAL source setup failed; continuing without "
                     "sound\n";
    }
}

AudioOutput::~AudioOutput() {
    if (context_ == nullptr) {
        return;
    }
    if (source_ != 0U) {
        alSourceStop(source_);
        // Stopping marks every queued buffer processed, so they can be
        // reclaimed before deletion rather than leaked with the context.
        ALint processed{};
        alGetSourcei(source_, AL_BUFFERS_PROCESSED, &processed);
        for (ALint index = 0; index < processed; ++index) {
            ALuint buffer{};
            alSourceUnqueueBuffers(source_, 1, &buffer);
            free_buffers_.push_back(buffer);
        }
        ALuint source = source_;
        alDeleteSources(1, &source);
    }
    if (!free_buffers_.empty()) {
        alDeleteBuffers(
            static_cast<ALsizei>(free_buffers_.size()), free_buffers_.data());
    }
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(static_cast<ALCcontext*>(context_));
    alcCloseDevice(static_cast<ALCdevice*>(device_));
}

void AudioOutput::stopAndDiscardQueued() noexcept {
    if (!ready_ || source_ == 0U) {
        return;
    }
    alSourceStop(source_);
    ALint queued{};
    alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
    for (ALint index = 0; index < queued; ++index) {
        ALuint buffer{};
        alSourceUnqueueBuffers(source_, 1, &buffer);
        if (buffer != 0U) {
            free_buffers_.push_back(buffer);
        }
    }
    // Do not let an expected, explicit stop count as device starvation when
    // the first post-movie buffer starts the source again.
    alSourceRewind(source_);
    alGetError();
}

std::size_t AudioOutput::queuedBufferCount() noexcept {
    if (!ready_) {
        return 0U;
    }
    submit({});
    ALint queued{};
    alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
    return queued > 0 ? static_cast<std::size_t>(queued) : 0U;
}

bool AudioOutput::canAcceptBuffer() noexcept {
    (void)queuedBufferCount();
    return ready_ && !free_buffers_.empty();
}

void AudioOutput::submit(std::span<const std::int16_t> samples) {
    if (!ready_) {
        return;
    }
    // Reclaim first, so a submission in the same call can use what the device
    // has just finished with.
    ALint processed{};
    alGetSourcei(source_, AL_BUFFERS_PROCESSED, &processed);
    for (ALint index = 0; index < processed; ++index) {
        ALuint buffer{};
        alSourceUnqueueBuffers(source_, 1, &buffer);
        free_buffers_.push_back(buffer);
    }

    if (!samples.empty()) {
        if (free_buffers_.empty()) {
            // Every buffer is still in flight, so the device is behind rather
            // than ahead. Dropping is better than blocking the thread that also
            // has to present the picture.
            ++dropped_submissions_;
        } else {
            const auto buffer = free_buffers_.back();
            free_buffers_.pop_back();
            alBufferData(
                buffer,
                AL_FORMAT_STEREO16,
                samples.data(),
                static_cast<ALsizei>(samples.size() * sizeof(std::int16_t)),
                static_cast<ALsizei>(sample_rate));
            alSourceQueueBuffers(source_, 1, &buffer);
        }
    }

    // A source that has run out of data stops on its own and has to be
    // restarted, which is the difference between a gap and permanent silence.
    ALint state{};
    alGetSourcei(source_, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING) {
        ALint queued{};
        alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
        if (queued > 0) {
            if (state == AL_STOPPED) {
                ++starved_;
            }
            alSourcePlay(source_);
        }
    }
}

} // namespace stuntmaster::presentation
