#pragma once

// Streamed music (US-057). The encoded bytes stay in memory and the decoder produces frames on demand, so a long track never becomes a
// decoded clip. Streams are read by the mixer on the audio thread; they touch no ECS state.

#include "engine/audio/audio_error.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include "engine/audio/byte_view.hpp"
#include <vector>

struct engine_vorbis_stream;

namespace engine::audio {

class MusicStream {
public:
    // Copies the bytes, so the caller's buffer may go away after the call.
    [[nodiscard]] static core::Result<MusicStream, AudioError> open(ByteView bytes);

    MusicStream(MusicStream&&) noexcept = default;
    MusicStream& operator=(MusicStream&&) noexcept = default;
    MusicStream(const MusicStream&) = delete;
    MusicStream& operator=(const MusicStream&) = delete;
    ~MusicStream() = default;

    [[nodiscard]] std::uint32_t channels() const noexcept {
        return channels_;
    }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept {
        return sample_rate_;
    }

    // Writes up to `frames` interleaved frames into `out` (frames * channels floats) and returns how many it wrote. A short count means
    // the stream ended. Rewind starts over from the first frame.
    [[nodiscard]] std::size_t read(float* out, std::size_t frames);
    void rewind();

private:
    struct VorbisCloser {
        void operator()(engine_vorbis_stream* stream) const noexcept;
    };

    enum class Kind {
        Wav,
        Ogg,
    };

    MusicStream() = default;

    std::vector<std::uint8_t> bytes_;  // the encoded file; the Vorbis decoder points into it
    Kind kind_ = Kind::Wav;
    std::uint32_t channels_ = 0;
    std::uint32_t sample_rate_ = 0;
    std::uint16_t bits_ = 0;
    bool is_float_ = false;
    std::size_t data_offset_ = 0;  // WAV: first sample byte
    std::size_t data_size_ = 0;    // WAV: sample bytes
    std::size_t cursor_ = 0;       // WAV: next byte to read, relative to data_offset_
    std::unique_ptr<engine_vorbis_stream, VorbisCloser> vorbis_;
};

}  // namespace engine::audio
