// The offline mixer and the audio engine (US-057): gain, buses, pan, distance attenuation, listener orientation, lifetime, looping,
// pitch and resampling, pause, owner release, and streamed music. The mixer renders directly, with no device, so every value is exact.

#include "engine/audio/audio_engine.hpp"
#include "engine/audio/cell_audio.hpp"
#include "engine/audio/mixer.hpp"
#include "engine/core/path.hpp"
#include "engine/world/partition.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using engine::audio::AudioClip;
using engine::audio::AudioEngine;
using engine::audio::AudioErrorKind;
using engine::audio::Bus;
using engine::audio::ByteView;
using engine::audio::Listener;
using engine::audio::Mixer;
using engine::audio::MusicDesc;
using engine::audio::MusicStream;
using engine::audio::OwnerId;
using engine::audio::VoiceDesc;
using engine::audio::VoiceId;
using engine::core::Path;

namespace {

constexpr std::uint32_t kRate = 48000;

std::shared_ptr<const AudioClip> constant_clip(std::size_t frames, float value, std::uint32_t rate = kRate) {
    auto clip = std::make_shared<AudioClip>();
    clip->channels = 1;
    clip->sample_rate = rate;
    clip->samples.assign(frames, value);
    return clip;
}

std::shared_ptr<const AudioClip> stereo_clip(std::size_t frames, float left, float right) {
    auto clip = std::make_shared<AudioClip>();
    clip->channels = 2;
    clip->sample_rate = kRate;
    for (std::size_t i = 0; i < frames; ++i) {
        clip->samples.push_back(left);
        clip->samples.push_back(right);
    }
    return clip;
}

// Renders `frames` frames and returns the stereo buffer.
std::vector<float> render(Mixer& mixer, std::size_t frames) {
    std::vector<float> out(2 * frames, 0.0f);
    mixer.render(out.data(), frames);
    return out;
}

VoiceId start(Mixer& mixer, std::shared_ptr<const AudioClip> clip, VoiceDesc desc = {}) {
    auto voice = mixer.play(std::move(clip), desc);
    REQUIRE(voice.ok());
    return voice.value();
}

std::vector<std::uint8_t> wav_bytes_of_constant(std::uint32_t frames, std::uint16_t sample) {
    std::vector<std::uint8_t> file;
    auto put = [&file](std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            file.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
        }
    };
    const std::uint32_t data_size = frames * 2;
    file.insert(file.end(), {'R', 'I', 'F', 'F'});
    put(36 + data_size, 4);
    file.insert(file.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put(16, 4);
    put(1, 2);
    put(1, 2);
    put(kRate, 4);
    put(kRate * 2, 4);
    put(2, 2);
    put(16, 2);
    file.insert(file.end(), {'d', 'a', 't', 'a'});
    put(data_size, 4);
    for (std::uint32_t i = 0; i < frames; ++i) {
        put(sample, 2);
    }
    return file;
}

}  // namespace

TEST_CASE("A centred mono voice is attenuated by its gain and the equal-power pan", "[unit]") {
    Mixer mixer(kRate);
    VoiceDesc desc;
    desc.volume = 0.5f;
    start(mixer, constant_clip(4, 0.5f), desc);
    const auto out = render(mixer, 1);
    // 0.5 (sample) * 0.5 (voice) * cos(pi/4) on each side.
    CHECK(out[0] == Catch::Approx(0.5f * 0.5f * std::cos(3.14159265f / 4.0f)).margin(1e-5));
    CHECK(out[1] == Catch::Approx(out[0]).margin(1e-6));
}

TEST_CASE("Bus and master gains multiply the voice gain", "[unit]") {
    Mixer mixer(kRate);
    mixer.set_bus_volume(Bus::Sfx, 0.5f);
    mixer.set_bus_volume(Bus::Master, 0.5f);
    start(mixer, constant_clip(4, 1.0f));
    const auto out = render(mixer, 1);
    // 1 * sfx 0.5 * master 0.5 * cos(pi/4)
    CHECK(out[0] == Catch::Approx(0.25f * std::cos(3.14159265f / 4.0f)).margin(1e-5));

    mixer.set_bus_volume(Bus::Master, 0.0f);
    const auto silent = render(mixer, 1);
    CHECK(silent[0] == 0.0f);
}

TEST_CASE("A music bus change does not touch sound effects", "[unit]") {
    Mixer mixer(kRate);
    mixer.set_bus_volume(Bus::Music, 0.0f);
    start(mixer, constant_clip(4, 1.0f));
    CHECK(render(mixer, 1)[0] > 0.5f);
}

TEST_CASE("Pan sends a mono voice to one side and a stereo balance attenuates the far channel", "[unit]") {
    Mixer mixer(kRate);
    VoiceDesc left;
    left.pan = -1.0f;
    start(mixer, constant_clip(4, 0.5f), left);
    auto out = render(mixer, 1);
    CHECK(out[0] == Catch::Approx(0.5f));
    CHECK(out[1] == Catch::Approx(0.0f).margin(1e-6));

    Mixer right_mixer(kRate);
    VoiceDesc right;
    right.pan = 1.0f;
    start(right_mixer, constant_clip(4, 0.5f), right);
    out = render(right_mixer, 1);
    CHECK(out[0] == Catch::Approx(0.0f).margin(1e-6));
    CHECK(out[1] == Catch::Approx(0.5f));

    Mixer stereo(kRate);
    VoiceDesc balance;
    balance.pan = -1.0f;
    start(stereo, stereo_clip(4, 0.5f, 0.25f), balance);
    out = render(stereo, 1);
    CHECK(out[0] == Catch::Approx(0.5f));
    CHECK(out[1] == Catch::Approx(0.0f).margin(1e-6));
}

TEST_CASE("Distance attenuation is full inside min, inverse between, and silent from max", "[unit]") {
    CHECK(engine::audio::attenuation(0.5, 1.0f, 50.0f) == 1.0f);
    CHECK(engine::audio::attenuation(1.0, 1.0f, 50.0f) == 1.0f);
    CHECK(engine::audio::attenuation(10.0, 1.0f, 50.0f) == Catch::Approx(0.1f));
    CHECK(engine::audio::attenuation(50.0, 1.0f, 50.0f) == 0.0f);
    CHECK(engine::audio::attenuation(80.0, 1.0f, 50.0f) == 0.0f);
}

TEST_CASE("A spatial voice pans by its direction from the listener and fades with distance", "[unit]") {
    Mixer mixer(kRate);
    VoiceDesc desc;
    desc.spatial = true;
    desc.position = glm::dvec3(10.0, 0.0, 0.0);  // ten metres to the listener's right (default forward -Z, up +Y)
    start(mixer, constant_clip(4, 0.5f), desc);
    const auto out = render(mixer, 1);
    CHECK(out[0] == Catch::Approx(0.0f).margin(1e-6));
    CHECK(out[1] == Catch::Approx(0.5f * 0.1f));
}

TEST_CASE("Turning the listener changes which side a spatial voice comes from", "[unit]") {
    Mixer mixer(kRate);
    Listener listener;
    listener.forward = glm::vec3(1.0f, 0.0f, 0.0f);  // facing +X, so the right-hand side is +Z
    mixer.set_listener(listener);
    VoiceDesc desc;
    desc.spatial = true;
    desc.position = glm::dvec3(0.0, 0.0, -5.0);
    start(mixer, constant_clip(4, 0.5f), desc);
    const auto out = render(mixer, 1);
    CHECK(out[0] == Catch::Approx(0.5f * 0.2f));
    CHECK(out[1] == Catch::Approx(0.0f).margin(1e-6));
}

TEST_CASE("A spatial voice beyond its max distance is silent while its lifetime runs", "[unit]") {
    Mixer mixer(kRate);
    VoiceDesc desc;
    desc.spatial = true;
    desc.position = glm::dvec3(100.0, 0.0, 0.0);
    desc.max_distance = 50.0f;
    const VoiceId voice = start(mixer, constant_clip(10, 0.5f), desc);
    const auto out = render(mixer, 1);
    CHECK(out[0] == 0.0f);
    CHECK(out[1] == 0.0f);
    CHECK(mixer.is_playing(voice));
}

TEST_CASE("A one-shot voice ends after its last frame and then emits silence", "[unit]") {
    Mixer mixer(kRate);
    const VoiceId voice = start(mixer, constant_clip(100, 0.5f));
    render(mixer, 50);
    CHECK(mixer.is_playing(voice));
    CHECK(mixer.voice_count() == 1);
    render(mixer, 50);
    CHECK_FALSE(mixer.is_playing(voice));
    CHECK(mixer.voice_count() == 0);
    const auto after = render(mixer, 4);
    for (float s : after) {
        CHECK(s == 0.0f);
    }
}

TEST_CASE("A looping voice keeps playing past the clip's length", "[unit]") {
    Mixer mixer(kRate);
    VoiceDesc desc;
    desc.loop = true;
    const VoiceId voice = start(mixer, constant_clip(100, 0.5f), desc);
    const auto out = render(mixer, 250);
    CHECK(mixer.is_playing(voice));
    CHECK(out[2 * 240] > 0.0f);
    CHECK(mixer.stop(voice));
    CHECK_FALSE(mixer.is_playing(voice));
    CHECK_FALSE(mixer.stop(voice));
}

TEST_CASE("Pitch and sample rate change the voice's length in output frames", "[unit]") {
    Mixer mixer(kRate);
    VoiceDesc octave;
    octave.pitch = 2.0f;
    const VoiceId fast = start(mixer, constant_clip(100, 0.5f), octave);
    render(mixer, 50);
    CHECK_FALSE(mixer.is_playing(fast));

    // A 24 kHz clip on a 48 kHz mixer is resampled, so it lasts twice as many output frames.
    const VoiceId resampled = start(mixer, constant_clip(100, 0.5f, 24000));
    render(mixer, 150);
    CHECK(mixer.is_playing(resampled));
    render(mixer, 60);
    CHECK_FALSE(mixer.is_playing(resampled));
}

TEST_CASE("Pause silences the mixer without advancing voices", "[unit]") {
    Mixer mixer(kRate);
    const VoiceId voice = start(mixer, constant_clip(100, 0.5f));
    mixer.set_paused(true);
    const auto silent = render(mixer, 80);
    for (float s : silent) {
        CHECK(s == 0.0f);
    }
    CHECK(mixer.is_playing(voice));
    mixer.set_paused(false);
    render(mixer, 50);
    CHECK(mixer.is_playing(voice));
    render(mixer, 50);
    CHECK_FALSE(mixer.is_playing(voice));
}

TEST_CASE("stop_owner releases only the voices that carry the owner tag", "[unit]") {
    Mixer mixer(kRate);
    VoiceDesc tagged;
    tagged.owner = 7;
    const VoiceId a = start(mixer, constant_clip(100, 0.1f), tagged);
    const VoiceId b = start(mixer, constant_clip(100, 0.1f), tagged);
    const VoiceId untagged = start(mixer, constant_clip(100, 0.1f));
    CHECK(mixer.stop_owner(7) == 2);
    CHECK_FALSE(mixer.is_playing(a));
    CHECK_FALSE(mixer.is_playing(b));
    CHECK(mixer.is_playing(untagged));
    CHECK(mixer.stop_owner(0) == 0);
}

TEST_CASE("Invalid voice descriptions are refused with InvalidArgument", "[unit]") {
    Mixer mixer(kRate);
    VoiceDesc master;
    master.bus = Bus::Master;
    auto refused = mixer.play(constant_clip(4, 0.5f), master);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().kind == AudioErrorKind::InvalidArgument);

    VoiceDesc zero_pitch;
    zero_pitch.pitch = 0.0f;
    CHECK_FALSE(mixer.play(constant_clip(4, 0.5f), zero_pitch).ok());

    VoiceDesc bad_range;
    bad_range.spatial = true;
    bad_range.min_distance = 10.0f;
    bad_range.max_distance = 5.0f;
    CHECK_FALSE(mixer.play(constant_clip(4, 0.5f), bad_range).ok());

    VoiceDesc wide_pan;
    wide_pan.pan = 2.0f;
    CHECK_FALSE(mixer.play(constant_clip(4, 0.5f), wide_pan).ok());

    CHECK_FALSE(mixer.play(constant_clip(0, 0.5f), VoiceDesc{}).ok());
}

TEST_CASE("Streamed music plays once, loops on request, and follows its bus", "[unit]") {
    const auto file = wav_bytes_of_constant(100, 16384);  // 0.5 for 100 frames
    auto opened = MusicStream::open(ByteView(file.data(), file.size()));
    REQUIRE(opened.ok());
    Mixer mixer(kRate);
    REQUIRE(mixer.play_music(std::move(opened.value()), MusicDesc{false, 1.0f}).ok());
    CHECK(mixer.music_playing());
    const auto out = render(mixer, 1);
    CHECK(out[0] == Catch::Approx(0.5f).margin(1e-4));
    CHECK(out[1] == Catch::Approx(out[0]).margin(1e-6));
    render(mixer, 200);
    CHECK_FALSE(mixer.music_playing());

    auto looped = MusicStream::open(ByteView(file.data(), file.size()));
    REQUIRE(looped.ok());
    REQUIRE(mixer.play_music(std::move(looped.value()), MusicDesc{true, 1.0f}).ok());
    render(mixer, 500);
    CHECK(mixer.music_playing());

    mixer.set_bus_volume(Bus::Music, 0.0f);
    CHECK(render(mixer, 1)[0] == 0.0f);
    mixer.stop_music();
    CHECK_FALSE(mixer.music_playing());
}

TEST_CASE("The audio engine loads, plays, and releases clips through one interface", "[unit]") {
    auto engine = AudioEngine::create_offline(kRate);
    CHECK_FALSE(engine->has_device());
    CHECK(engine->device_name().empty());

    AudioClip clip;
    clip.channels = 1;
    clip.sample_rate = kRate;
    clip.samples.assign(100, 0.25f);
    auto id = engine->add_clip(clip);
    REQUIRE(id.ok());

    VoiceDesc owned;
    owned.owner = 42;
    auto voice = engine->play(id.value(), owned);
    REQUIRE(voice.ok());
    CHECK(engine->is_playing(voice.value()));
    CHECK(engine->voice_count() == 1);
    CHECK(engine->set_voice_position(voice.value(), glm::dvec3(1.0)));
    CHECK(engine->stop_owner(42) == 1);
    CHECK_FALSE(engine->is_playing(voice.value()));

    auto unknown = engine->play(99, VoiceDesc{});
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().kind == AudioErrorKind::InvalidArgument);

    auto empty = engine->add_clip(AudioClip{});
    CHECK_FALSE(empty.ok());

    auto missing = engine->load_clip(Path::from_utf8(std::string(ENGINE_TEST_FIXTURE_DIR) + "/audio/missing.ogg").value());
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().kind == AudioErrorKind::FileUnreadable);

    auto fixture = engine->load_clip(Path::from_utf8(std::string(ENGINE_TEST_FIXTURE_DIR) + "/audio/tone_mono.ogg").value());
    REQUIRE(fixture.ok());
    REQUIRE(engine->play(fixture.value(), VoiceDesc{}).ok());

    auto music = engine->play_music_file(Path::from_utf8(std::string(ENGINE_TEST_FIXTURE_DIR) + "/audio/missing.wav").value(), {});
    REQUIRE_FALSE(music.ok());
    CHECK(music.error().kind == AudioErrorKind::FileUnreadable);

    auto stream = engine->play_music_file(Path::from_utf8(std::string(ENGINE_TEST_FIXTURE_DIR) + "/audio/tone_stereo.ogg").value(), {});
    REQUIRE(stream.ok());
    CHECK(engine->music_playing());
    engine->stop_music();
    CHECK_FALSE(engine->music_playing());
}

TEST_CASE("Pausing the audio engine silences its render output", "[unit]") {
    auto engine = AudioEngine::create_offline(kRate);
    AudioClip clip;
    clip.channels = 1;
    clip.sample_rate = kRate;
    clip.samples.assign(100, 0.5f);
    auto id = engine->add_clip(clip);
    REQUIRE(id.ok());
    REQUIRE(engine->play(id.value(), VoiceDesc{}).ok());
    engine->set_paused(true);
    CHECK(engine->paused());
    float out[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    engine->render(out, 4);
    for (float s : out) {
        CHECK(s == 0.0f);
    }
    engine->set_paused(false);
    engine->set_bus_volume(Bus::Master, 1.0f);
    CHECK(engine->bus_volume(Bus::Master) == 1.0f);
}

TEST_CASE("A cell's unload releases its voices and leaves other owners playing", "[unit]") {
    auto engine = AudioEngine::create_offline(kRate);
    AudioClip clip;
    clip.channels = 1;
    clip.sample_rate = kRate;
    clip.samples.assign(1000, 0.5f);
    auto id = engine->add_clip(clip);
    REQUIRE(id.ok());

    const engine::world::CellCoord inside{3, -2};
    const engine::world::CellCoord other{-2, 3};
    engine::audio::CellAudioSubscriber subscriber(*engine);

    VoiceDesc in_cell;
    in_cell.owner = engine::audio::CellAudioSubscriber::cell_owner(inside);
    VoiceDesc in_other_cell;
    in_other_cell.owner = engine::audio::CellAudioSubscriber::cell_owner(other);
    VoiceDesc scripted;
    scripted.owner = 0x1234;
    auto a = engine->play(id.value(), in_cell);
    auto b = engine->play(id.value(), in_other_cell);
    auto c = engine->play(id.value(), scripted);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(c.ok());
    REQUIRE(engine->voice_count() == 3);

    auto cell = std::make_shared<engine::world::WorldCell>();
    cell->coord = inside;
    subscriber.on_loaded(cell);
    CHECK(engine->voice_count() == 3);

    subscriber.on_unloaded(cell);
    CHECK(engine->voice_count() == 2);
    CHECK_FALSE(engine->is_playing(a.value()));
    CHECK(engine->is_playing(b.value()));
    CHECK(engine->is_playing(c.value()));

    subscriber.on_unloaded(nullptr);
    CHECK(engine->voice_count() == 2);
}

TEST_CASE("Cell owners are non-zero, distinct per cell, and clear of script owners", "[unit]") {
    using engine::audio::CellAudioSubscriber;
    const engine::world::CellCoord cells[] = {{0, 0}, {-1, -1}, {1, 0}, {0, 1}, {-1, 0}, {0, -1},
                                              {1 << 20, -(1 << 20)}, {-(1 << 20), 1 << 20}};
    for (std::size_t i = 0; i < std::size(cells); ++i) {
        const OwnerId owner = CellAudioSubscriber::cell_owner(cells[i]);
        CHECK(owner != 0);
        CHECK((owner & (std::uint64_t{1} << 63)) != 0);
        for (std::size_t j = i + 1; j < std::size(cells); ++j) {
            CHECK(owner != CellAudioSubscriber::cell_owner(cells[j]));
        }
    }
}
