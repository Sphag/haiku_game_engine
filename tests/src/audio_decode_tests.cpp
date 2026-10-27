// The audio decoders (US-057) on the CPU: WAV sample formats, the refusal messages for unsupported subtypes, the Ogg fixtures, and the
// streamed music decoder. The fixtures are the synthetic tones in tests/fixtures/audio (README.md lists how they were made).

#include "engine/audio/audio_clip.hpp"
#include "engine/audio/byte_view.hpp"
#include "engine/audio/music_stream.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using engine::audio::AudioClip;
using engine::audio::AudioContainer;
using engine::audio::AudioErrorKind;
using engine::audio::ByteView;
using engine::audio::MusicStream;
using engine::core::Path;

namespace {

void put_le(std::vector<std::uint8_t>& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

// A RIFF/WAVE file with one fmt chunk and one data chunk. `fmt_extra` is appended to the fmt body (for the extensible form).
std::vector<std::uint8_t> make_wav(std::uint16_t format_tag, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits,
                                   const std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& fmt_extra = {},
                                   std::uint32_t declared_data_size = 0) {
    std::vector<std::uint8_t> fmt;
    put_le(fmt, format_tag, 2);
    put_le(fmt, channels, 2);
    put_le(fmt, rate, 4);
    put_le(fmt, rate * channels * (bits / 8u), 4);
    put_le(fmt, channels * (bits / 8u), 2);
    put_le(fmt, bits, 2);
    fmt.insert(fmt.end(), fmt_extra.begin(), fmt_extra.end());

    std::vector<std::uint8_t> file;
    file.insert(file.end(), {'R', 'I', 'F', 'F'});
    put_le(file, static_cast<std::uint32_t>(4 + 8 + fmt.size() + 8 + data.size()), 4);
    file.insert(file.end(), {'W', 'A', 'V', 'E'});
    file.insert(file.end(), {'f', 'm', 't', ' '});
    put_le(file, static_cast<std::uint32_t>(fmt.size()), 4);
    file.insert(file.end(), fmt.begin(), fmt.end());
    file.insert(file.end(), {'d', 'a', 't', 'a'});
    put_le(file, declared_data_size != 0 ? declared_data_size : static_cast<std::uint32_t>(data.size()), 4);
    file.insert(file.end(), data.begin(), data.end());
    return file;
}

ByteView view(const std::vector<std::uint8_t>& bytes) {
    return ByteView(bytes.data(), bytes.size());
}

std::vector<std::uint8_t> read_fixture(const std::string& name) {
    const auto path = Path::from_utf8(std::string(ENGINE_TEST_FIXTURE_DIR) + "/audio/" + name);
    REQUIRE(path.ok());
    const auto text = engine::core::read_file(path.value());
    REQUIRE(text.ok());
    return std::vector<std::uint8_t>(text.value().begin(), text.value().end());
}

float peak(const AudioClip& clip) {
    float value = 0.0f;
    for (float s : clip.samples) {
        value = std::max(value, std::fabs(s));
    }
    return value;
}

}  // namespace

TEST_CASE("16-bit PCM WAV decodes to floats in [-1, 1]", "[unit]") {
    const std::vector<std::uint8_t> data = {0x00, 0x00, 0x00, 0x40, 0x00, 0x80, 0xFF, 0x7F};  // 0, 0.5, -1, 32767/32768
    const auto file = make_wav(1, 1, 48000, 16, data);
    const auto clip = engine::audio::decode_wav(view(file));
    REQUIRE(clip.ok());
    CHECK(clip.value().channels == 1);
    CHECK(clip.value().sample_rate == 48000);
    REQUIRE(clip.value().samples.size() == 4);
    CHECK(clip.value().samples[0] == 0.0f);
    CHECK(clip.value().samples[1] == Catch::Approx(0.5f));
    CHECK(clip.value().samples[2] == Catch::Approx(-1.0f));
    CHECK(clip.value().samples[3] == Catch::Approx(32767.0f / 32768.0f));
}

TEST_CASE("8-bit PCM WAV is unsigned and centred on 128", "[unit]") {
    const auto file = make_wav(1, 1, 8000, 8, {128, 255, 0});
    const auto clip = engine::audio::decode_wav(view(file));
    REQUIRE(clip.ok());
    REQUIRE(clip.value().samples.size() == 3);
    CHECK(clip.value().samples[0] == 0.0f);
    CHECK(clip.value().samples[1] == Catch::Approx(127.0f / 128.0f));
    CHECK(clip.value().samples[2] == Catch::Approx(-1.0f));
}

TEST_CASE("24-bit PCM WAV sign-extends the three bytes", "[unit]") {
    // -1 is FF FF FF; 0x400000 is 00 00 40 (one half of full scale).
    const std::vector<std::uint8_t> data = {0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x40};
    const auto file = make_wav(1, 1, 48000, 24, data);
    const auto clip = engine::audio::decode_wav(view(file));
    REQUIRE(clip.ok());
    REQUIRE(clip.value().samples.size() == 2);
    CHECK(clip.value().samples[0] == Catch::Approx(-1.0f / 8388608.0f));
    CHECK(clip.value().samples[1] == Catch::Approx(0.5f));
}

TEST_CASE("32-bit float stereo WAV keeps its interleaved values", "[unit]") {
    std::vector<std::uint8_t> data;
    for (float value : {0.25f, -0.5f, 0.75f, 0.0f}) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        put_le(data, bits, 4);
    }
    const auto file = make_wav(3, 2, 44100, 32, data);
    const auto clip = engine::audio::decode_wav(view(file));
    REQUIRE(clip.ok());
    CHECK(clip.value().channels == 2);
    CHECK(clip.value().frame_count() == 2);
    CHECK(clip.value().samples[0] == Catch::Approx(0.25f));
    CHECK(clip.value().samples[1] == Catch::Approx(-0.5f));
    CHECK(clip.value().samples[2] == Catch::Approx(0.75f));
}

TEST_CASE("WAVE_FORMAT_EXTENSIBLE with a PCM sub-format decodes", "[unit]") {
    std::vector<std::uint8_t> extra(24, 0);
    extra[0] = 22;  // cbSize
    extra[2] = 16;  // valid bits
    extra[8] = 1;   // sub-format GUID starts with the PCM tag 1
    extra[9] = 0;
    extra[10] = 0;
    extra[11] = 0;
    const auto file = make_wav(0xFFFE, 1, 48000, 16, {0x00, 0x40}, extra);
    const auto clip = engine::audio::decode_wav(view(file));
    REQUIRE(clip.ok());
    REQUIRE(clip.value().samples.size() == 1);
    CHECK(clip.value().samples[0] == Catch::Approx(0.5f));
}

TEST_CASE("Unsupported WAV subtypes are refused with the subtype named", "[unit]") {
    const auto six_channels = engine::audio::decode_wav(view(make_wav(1, 6, 48000, 16, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})));
    REQUIRE_FALSE(six_channels.ok());
    CHECK(six_channels.error().kind == AudioErrorKind::UnsupportedFormat);
    CHECK(six_channels.error().message.find("6 channels") != std::string::npos);

    const auto a_law = engine::audio::decode_wav(view(make_wav(6, 1, 8000, 8, {0x55})));
    REQUIRE_FALSE(a_law.ok());
    CHECK(a_law.error().kind == AudioErrorKind::UnsupportedFormat);
    CHECK(a_law.error().message.find("format tag 6") != std::string::npos);

    const auto twelve_bit = engine::audio::decode_wav(view(make_wav(1, 1, 48000, 12, {0, 0})));
    REQUIRE_FALSE(twelve_bit.ok());
    CHECK(twelve_bit.error().message.find("12 bits") != std::string::npos);
}

TEST_CASE("A WAV whose data chunk runs past the file is reported as truncated", "[unit]") {
    const auto file = make_wav(1, 1, 48000, 16, {0, 0, 0, 0}, {}, 100);
    const auto clip = engine::audio::decode_wav(view(file));
    REQUIRE_FALSE(clip.ok());
    CHECK(clip.error().kind == AudioErrorKind::Truncated);
}

TEST_CASE("Bytes that are not a RIFF/WAVE or Ogg file are rejected", "[unit]") {
    const std::vector<std::uint8_t> junk = {'M', 'P', '3', '!', 0, 0, 0, 0, 0, 0, 0, 0};
    const auto container = engine::audio::detect_container(view(junk));
    REQUIRE_FALSE(container.ok());
    CHECK(container.error().kind == AudioErrorKind::UnsupportedFormat);
    const auto clip = engine::audio::decode_audio(view(junk));
    REQUIRE_FALSE(clip.ok());

    const std::vector<std::uint8_t> riff_without_wave = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'A', 'V', 'I', ' '};
    const auto no_wave = engine::audio::decode_wav(view(riff_without_wave));
    REQUIRE_FALSE(no_wave.ok());
    CHECK(no_wave.error().kind == AudioErrorKind::Corrupt);
}

TEST_CASE("Ogg fixtures decode to their channel count and rate", "[unit]") {
    const auto mono = engine::audio::decode_audio(view(read_fixture("tone_mono.ogg")));
    REQUIRE(mono.ok());
    CHECK(mono.value().channels == 1);
    CHECK(mono.value().sample_rate == 22050);
    // 0.25 s at 22.05 kHz; the encoder may add a packet of padding.
    CHECK(mono.value().frame_count() >= 5400);
    CHECK(mono.value().frame_count() <= 6200);
    CHECK(peak(mono.value()) > 0.05f);

    const auto stereo = engine::audio::decode_audio(view(read_fixture("tone_stereo.ogg")));
    REQUIRE(stereo.ok());
    CHECK(stereo.value().channels == 2);
    CHECK(stereo.value().sample_rate == 44100);
    CHECK(stereo.value().frame_count() >= 21000);
    CHECK(stereo.value().frame_count() <= 23000);
}

TEST_CASE("The bundled WAV fixtures decode to their documented formats", "[unit]") {
    const auto mono = engine::audio::decode_audio(view(read_fixture("tone_mono_s16.wav")));
    REQUIRE(mono.ok());
    CHECK(mono.value().channels == 1);
    CHECK(mono.value().sample_rate == 48000);
    CHECK(mono.value().frame_count() == 12000);
    // The lavfi sine is generated at 1/8 of full scale.
    CHECK(peak(mono.value()) == Catch::Approx(0.125f).margin(0.01f));

    const auto stereo = engine::audio::decode_audio(view(read_fixture("tone_stereo_f32.wav")));
    REQUIRE(stereo.ok());
    CHECK(stereo.value().channels == 2);
    CHECK(stereo.value().sample_rate == 44100);
    CHECK(stereo.value().frame_count() == 11025);
}

TEST_CASE("A damaged Ogg stream is refused rather than decoded", "[unit]") {
    std::vector<std::uint8_t> bytes = {'O', 'g', 'g', 'S'};
    bytes.resize(64, 0x5A);
    const auto clip = engine::audio::decode_ogg(view(bytes));
    REQUIRE_FALSE(clip.ok());
    CHECK(clip.error().kind == AudioErrorKind::Corrupt);
}

TEST_CASE("A missing audio file reports FileUnreadable", "[unit]") {
    const auto path = Path::from_utf8(std::string(ENGINE_TEST_FIXTURE_DIR) + "/audio/does_not_exist.wav");
    REQUIRE(path.ok());
    const auto clip = engine::audio::load_audio_clip(path.value());
    REQUIRE_FALSE(clip.ok());
    CHECK(clip.error().kind == AudioErrorKind::FileUnreadable);
}

TEST_CASE("A music stream reads a WAV in blocks, ends, and rewinds to the same start", "[unit]") {
    std::vector<std::uint8_t> data;
    for (int i = 0; i < 10; ++i) {
        put_le(data, static_cast<std::uint32_t>(static_cast<std::uint16_t>(i * 1000)), 2);
    }
    const auto file = make_wav(1, 1, 22050, 16, data);
    auto opened = MusicStream::open(view(file));
    REQUIRE(opened.ok());
    MusicStream stream = std::move(opened.value());
    CHECK(stream.channels() == 1);
    CHECK(stream.sample_rate() == 22050);

    float first[4] = {};
    REQUIRE(stream.read(first, 4) == 4);
    float rest[16] = {};
    REQUIRE(stream.read(rest, 16) == 6);
    CHECK(stream.read(rest, 1) == 0);

    stream.rewind();
    float again[1] = {};
    REQUIRE(stream.read(again, 1) == 1);
    CHECK(again[0] == first[0]);
}

TEST_CASE("A music stream over Ogg yields the same frames as the clip decoder", "[unit]") {
    const auto bytes = read_fixture("tone_stereo.ogg");
    const auto clip = engine::audio::decode_ogg(view(bytes));
    REQUIRE(clip.ok());
    auto opened = MusicStream::open(view(bytes));
    REQUIRE(opened.ok());
    MusicStream stream = std::move(opened.value());
    CHECK(stream.channels() == 2);

    std::size_t total = 0;
    std::vector<float> block(2 * 512);
    for (;;) {
        const std::size_t got = stream.read(block.data(), 512);
        if (got == 0) {
            break;
        }
        total += got;
    }
    CHECK(total == clip.value().frame_count());

    stream.rewind();
    float first[2] = {};
    REQUIRE(stream.read(first, 1) == 1);
    CHECK(first[0] == Catch::Approx(clip.value().samples[0]).margin(1e-6));
}
