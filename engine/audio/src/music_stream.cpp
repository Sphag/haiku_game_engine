#include "engine/audio/music_stream.hpp"

#include "native/vorbis_stream.h"
#include "wav_format.hpp"

#include <cstring>
#include <string>

namespace engine::audio {
namespace {

using core::Result;

core::Failure<AudioError> error(AudioErrorKind kind, std::string message) {
    return core::failure(AudioError{kind, std::move(message)});
}

}  // namespace

void MusicStream::VorbisCloser::operator()(engine_vorbis_stream* stream) const noexcept {
    engine_vorbis_close(stream);
}

Result<MusicStream, AudioError> MusicStream::open(ByteView bytes) {
    MusicStream stream;
    stream.bytes_.assign(bytes.begin(), bytes.end());
    const ByteView owned(stream.bytes_.data(), stream.bytes_.size());

    if (owned.size() >= 4 && std::memcmp(owned.data(), "RIFF", 4) == 0) {
        auto parsed = detail::parse_wav(owned);
        if (!parsed.ok()) {
            return core::failure(parsed.error());
        }
        const detail::WavLayout& layout = parsed.value();
        stream.kind_ = Kind::Wav;
        stream.channels_ = layout.channels;
        stream.sample_rate_ = layout.sample_rate;
        stream.bits_ = layout.bits;
        stream.is_float_ = layout.is_float;
        stream.data_offset_ = layout.data_offset;
        stream.data_size_ = layout.data_size;
        return core::success(std::move(stream));
    }
    if (owned.size() >= 4 && std::memcmp(owned.data(), "OggS", 4) == 0) {
        std::uint32_t channels = 0;
        std::uint32_t sample_rate = 0;
        std::uint64_t total_frames = 0;
        engine_vorbis_stream* decoder = engine_vorbis_open(owned.data(), owned.size(), &channels, &sample_rate, &total_frames);
        if (decoder == nullptr) {
            return error(AudioErrorKind::Corrupt, "Ogg file is not a valid Vorbis stream");
        }
        if (channels != 1 && channels != 2) {
            engine_vorbis_close(decoder);
            return error(AudioErrorKind::UnsupportedFormat,
                         "Ogg has " + std::to_string(channels) + " channels; only mono and stereo are supported");
        }
        stream.kind_ = Kind::Ogg;
        stream.channels_ = channels;
        stream.sample_rate_ = sample_rate;
        stream.vorbis_.reset(decoder);
        return core::success(std::move(stream));
    }
    return error(AudioErrorKind::UnsupportedFormat, "music file is neither WAV (RIFF) nor Ogg (OggS)");
}

std::size_t MusicStream::read(float* out, std::size_t frames) {
    if (kind_ == Kind::Ogg) {
        return engine_vorbis_read(vorbis_.get(), out, frames);
    }
    // WAV: convert whole frames from the cursor. A partial frame at the end is dropped.
    const std::size_t bytes_per_sample = bits_ / 8u;
    const std::size_t frame_bytes = bytes_per_sample * channels_;
    const detail::WavLayout layout{channels_, sample_rate_, bits_, is_float_, data_offset_, data_size_};
    std::size_t written = 0;
    while (written < frames && cursor_ + frame_bytes <= data_size_) {
        const std::uint8_t* frame = bytes_.data() + data_offset_ + cursor_;
        for (std::uint32_t c = 0; c < channels_; ++c) {
            out[written * channels_ + c] = detail::wav_sample(frame + c * bytes_per_sample, layout);
        }
        cursor_ += frame_bytes;
        ++written;
    }
    return written;
}

void MusicStream::rewind() {
    cursor_ = 0;
    if (kind_ == Kind::Ogg) {
        engine_vorbis_seek_start(vorbis_.get());
    }
}

}  // namespace engine::audio
