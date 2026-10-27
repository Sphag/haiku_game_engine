// Native smoke test for the playback device (US-057). The test does not assume that the machine has a sound device. It accepts a
// device that opens and plays, or a clean DeviceUnavailable error with a message, and checks that the offline engine still works after
// either outcome.

#include "engine/audio/audio_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using engine::audio::AudioClip;
using engine::audio::AudioEngine;
using engine::audio::AudioErrorKind;
using engine::audio::VoiceDesc;

TEST_CASE("The playback device either starts and plays, or reports that it is unavailable", "[integration]") {
    auto device = AudioEngine::open_device(AudioEngine::kDefaultSampleRate);
    if (device.ok()) {
        AudioEngine& engine = *device.value();
        CHECK(engine.has_device());
        CHECK(engine.sample_rate() != 0);
        INFO("device: " << engine.device_name());
        AudioClip clip;
        clip.channels = 1;
        clip.sample_rate = AudioEngine::kDefaultSampleRate;
        clip.samples.assign(AudioEngine::kDefaultSampleRate / 10, 0.1f);
        auto id = engine.add_clip(clip);
        REQUIRE(id.ok());
        auto voice = engine.play(id.value(), VoiceDesc{});
        REQUIRE(voice.ok());
        // Give the device thread a few callbacks, then check the voice is still advancing or has finished cleanly.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK_FALSE(engine.is_playing(voice.value()));
    } else {
        CHECK(device.error().kind == AudioErrorKind::DeviceUnavailable);
        CHECK_FALSE(device.error().message.empty());
        INFO("no device: " << device.error().message);
    }

    auto offline = AudioEngine::create_offline();
    CHECK_FALSE(offline->has_device());
    float out[8] = {};
    offline->render(out, 4);
    for (float s : out) {
        CHECK(s == 0.0f);
    }
}
