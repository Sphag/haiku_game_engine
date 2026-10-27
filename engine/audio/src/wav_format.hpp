#pragma once

// The WAV layout shared by the whole-clip decoder and the streamed music decoder (US-057). Internal to engine_audio.

#include "engine/audio/audio_error.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include "engine/audio/byte_view.hpp"

namespace engine::audio::detail {

struct WavLayout {
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bits = 0;
    bool is_float = false;
    std::size_t data_offset = 0;  // first sample byte
    std::size_t data_size = 0;    // sample bytes; whole frames only are decoded
};

// Walks the RIFF chunks up to the data chunk. Refuses anything it does not decode, and names the subtype in the message.
[[nodiscard]] core::Result<WavLayout, AudioError> parse_wav(ByteView bytes);

// Little-endian integer of `bytes` bytes (1 to 4).
[[nodiscard]] std::uint32_t read_le(const std::uint8_t* p, int bytes) noexcept;

// Converts one sample of `layout` at `sample` to a float in [-1, 1].
[[nodiscard]] float wav_sample(const std::uint8_t* sample, const WavLayout& layout) noexcept;

}  // namespace engine::audio::detail
