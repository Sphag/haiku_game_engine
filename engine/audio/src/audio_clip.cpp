#include "engine/audio/audio_clip.hpp"

#include "engine/core/file_io.hpp"
#include "native/vorbis_stream.h"
#include "wav_format.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace engine::audio {
namespace {

using core::Result;

constexpr std::size_t kDecodeBlockFrames = 4096;
constexpr std::uint64_t kMaxReserveFrames = std::uint64_t{1} << 22;

core::Failure<AudioError> error(AudioErrorKind kind, std::string message) {
    return core::failure(AudioError{kind, std::move(message)});
}

bool tag_is(ByteView bytes, std::size_t at, const char* tag) {
    return std::memcmp(bytes.data() + at, tag, 4) == 0;
}

}  // namespace

namespace detail {

std::uint32_t read_le(const std::uint8_t* p, int bytes) noexcept {
    std::uint32_t value = 0;
    for (int i = bytes - 1; i >= 0; --i) {
        value = (value << 8) | p[i];
    }
    return value;
}

Result<WavLayout, AudioError> parse_wav(ByteView bytes) {
    if (bytes.size() < 12 || !tag_is(bytes, 0, "RIFF") || !tag_is(bytes, 8, "WAVE")) {
        return error(AudioErrorKind::Corrupt, "WAV file has no RIFF/WAVE header");
    }
    WavLayout layout;
    bool have_format = false;
    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::uint8_t* header = bytes.data() + pos;
        const std::size_t body = pos + 8;
        const std::size_t size = read_le(header + 4, 4);
        if (size > bytes.size() - body) {
            return error(AudioErrorKind::Truncated, "WAV chunk runs past the end of the file");
        }
        if (tag_is(bytes, pos, "fmt ")) {
            if (size < 16) {
                return error(AudioErrorKind::Corrupt, "WAV fmt chunk is too short");
            }
            const std::uint8_t* fmt = bytes.data() + body;
            std::uint32_t format_tag = read_le(fmt, 2);
            layout.channels = read_le(fmt + 2, 2);
            layout.sample_rate = read_le(fmt + 4, 4);
            layout.bits = static_cast<std::uint16_t>(read_le(fmt + 14, 2));
            if (format_tag == 0xFFFE) {  // WAVE_FORMAT_EXTENSIBLE: the real tag opens the sub-format GUID
                if (size < 40) {
                    return error(AudioErrorKind::Corrupt, "WAV extensible fmt chunk is too short");
                }
                format_tag = read_le(fmt + 24, 2);
            }
            if (format_tag != 1 && format_tag != 3) {
                return error(AudioErrorKind::UnsupportedFormat,
                             "WAV format tag " + std::to_string(format_tag) + " is not supported; PCM and IEEE float are");
            }
            layout.is_float = format_tag == 3;
            if (layout.is_float ? layout.bits != 32 : !(layout.bits == 8 || layout.bits == 16 || layout.bits == 24 || layout.bits == 32)) {
                return error(AudioErrorKind::UnsupportedFormat, "WAV sample size " + std::to_string(layout.bits) + " bits is not supported");
            }
            if (layout.channels != 1 && layout.channels != 2) {
                return error(AudioErrorKind::UnsupportedFormat,
                             "WAV has " + std::to_string(layout.channels) + " channels; only mono and stereo are supported");
            }
            if (layout.sample_rate == 0) {
                return error(AudioErrorKind::Corrupt, "WAV sample rate is zero");
            }
            have_format = true;
        } else if (tag_is(bytes, pos, "data")) {
            if (!have_format) {
                return error(AudioErrorKind::Corrupt, "WAV data chunk comes before the fmt chunk");
            }
            layout.data_offset = body;
            layout.data_size = size;
            return core::success(layout);
        }
        pos = body + size + (size & 1);  // chunks are word aligned
    }
    return error(AudioErrorKind::Corrupt, "WAV file has no data chunk");
}

float wav_sample(const std::uint8_t* sample, const WavLayout& layout) noexcept {
    if (layout.is_float) {
        const std::uint32_t bits = read_le(sample, 4);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    switch (layout.bits) {
        case 8:
            return (static_cast<float>(sample[0]) - 128.0f) / 128.0f;
        case 16:
            return static_cast<float>(static_cast<std::int16_t>(read_le(sample, 2))) / 32768.0f;
        case 24: {
            // Sign-extend the 24-bit value through a 32-bit word.
            const std::uint32_t word = read_le(sample, 3) << 8;
            return static_cast<float>(static_cast<std::int32_t>(word)) / 2147483648.0f;
        }
        default:
            return static_cast<float>(static_cast<std::int32_t>(read_le(sample, 4))) / 2147483648.0f;
    }
}

}  // namespace detail

Result<AudioContainer, AudioError> detect_container(ByteView bytes) {
    if (bytes.size() >= 4 && tag_is(bytes, 0, "RIFF")) {
        return core::success(AudioContainer::Wav);
    }
    if (bytes.size() >= 4 && tag_is(bytes, 0, "OggS")) {
        return core::success(AudioContainer::Ogg);
    }
    return error(AudioErrorKind::UnsupportedFormat, "audio file is neither WAV (RIFF) nor Ogg (OggS)");
}

Result<AudioClip, AudioError> decode_wav(ByteView bytes) {
    auto parsed = detail::parse_wav(bytes);
    if (!parsed.ok()) {
        return core::failure(parsed.error());
    }
    const detail::WavLayout& layout = parsed.value();
    const std::size_t frame_bytes = (layout.bits / 8u) * layout.channels;
    const std::size_t frames = layout.data_size / frame_bytes;
    AudioClip clip;
    clip.channels = layout.channels;
    clip.sample_rate = layout.sample_rate;
    clip.samples.resize(frames * layout.channels);
    const std::uint8_t* data = bytes.data() + layout.data_offset;
    for (std::size_t i = 0; i < clip.samples.size(); ++i) {
        clip.samples[i] = detail::wav_sample(data + i * (layout.bits / 8u), layout);
    }
    return core::success(std::move(clip));
}

Result<AudioClip, AudioError> decode_ogg(ByteView bytes) {
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t total_frames = 0;
    engine_vorbis_stream* stream = engine_vorbis_open(bytes.data(), bytes.size(), &channels, &sample_rate, &total_frames);
    if (stream == nullptr) {
        return error(AudioErrorKind::Corrupt, "Ogg file is not a valid Vorbis stream");
    }
    if (channels != 1 && channels != 2) {
        engine_vorbis_close(stream);
        return error(AudioErrorKind::UnsupportedFormat,
                     "Ogg has " + std::to_string(channels) + " channels; only mono and stereo are supported");
    }
    AudioClip clip;
    clip.channels = channels;
    clip.sample_rate = sample_rate;
    // The declared length comes from the file, so a damaged stream must not reserve more than a few minutes of audio up front.
    clip.samples.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(total_frames, kMaxReserveFrames)) * channels);
    std::vector<float> block(kDecodeBlockFrames * channels);
    for (;;) {
        const std::size_t frames = engine_vorbis_read(stream, block.data(), kDecodeBlockFrames);
        if (frames == 0) {
            break;
        }
        clip.samples.insert(clip.samples.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(frames * channels));
    }
    engine_vorbis_close(stream);
    return core::success(std::move(clip));
}

Result<AudioClip, AudioError> decode_audio(ByteView bytes) {
    auto container = detect_container(bytes);
    if (!container.ok()) {
        return core::failure(container.error());
    }
    return container.value() == AudioContainer::Wav ? decode_wav(bytes) : decode_ogg(bytes);
}

Result<AudioClip, AudioError> load_audio_clip(const core::Path& path) {
    auto file = core::read_file(path);
    if (!file.ok()) {
        return error(AudioErrorKind::FileUnreadable, core::describe(file.error()));
    }
    const std::string& text = file.value();
    const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
    return decode_audio(ByteView(data, text.size()));
}

}  // namespace engine::audio
