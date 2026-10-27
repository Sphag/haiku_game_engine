#pragma once

// Decoded audio clips (US-057). A clip holds float samples in [-1, 1], interleaved, and is decoded whole. Short effects use clips; music
// uses MusicStream, which decodes as it plays.

#include "engine/audio/audio_error.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include "engine/audio/byte_view.hpp"
#include <vector>

namespace engine::audio {

struct AudioClip {
    std::uint32_t channels = 0;     // 1 (mono) or 2 (stereo)
    std::uint32_t sample_rate = 0;  // frames per second
    std::vector<float> samples;     // interleaved, channels per frame

    [[nodiscard]] std::size_t frame_count() const noexcept {
        return channels == 0 ? 0 : samples.size() / channels;
    }
};

enum class AudioContainer {
    Wav,
    Ogg,
};

// Recognises the container from its first bytes ("RIFF" or "OggS"). Anything else is UnsupportedFormat.
[[nodiscard]] core::Result<AudioContainer, AudioError> detect_container(ByteView bytes);

// WAV: PCM with 8, 16, 24, or 32 bit samples, or IEEE float with 32 bits, in the plain or WAVE_FORMAT_EXTENSIBLE form. Mono and stereo only.
[[nodiscard]] core::Result<AudioClip, AudioError> decode_wav(ByteView bytes);

// Ogg Vorbis, mono or stereo. The decoder works in float internally.
[[nodiscard]] core::Result<AudioClip, AudioError> decode_ogg(ByteView bytes);

// Detects the container, then decodes it.
[[nodiscard]] core::Result<AudioClip, AudioError> decode_audio(ByteView bytes);

// Reads the file and decodes it. The file's bytes are read once and released before the call returns.
[[nodiscard]] core::Result<AudioClip, AudioError> load_audio_clip(const core::Path& path);

}  // namespace engine::audio
