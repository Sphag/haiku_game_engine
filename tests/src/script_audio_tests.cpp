// The Lua audio module (US-057) on the CPU: scripts load clips, start and stop voices, move listeners, and pause the mix. The tests run
// against the offline engine. They check the controlled errors and that a script's voices leave with its ScriptEngine.

#include "engine/audio/audio_engine.hpp"
#include "engine/core/path.hpp"
#include "engine/script/script_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using engine::audio::AudioEngine;
using engine::audio::AudioClip;
using engine::audio::VoiceDesc;
using engine::script::ScriptEngine;
using engine::script::ScriptServices;

namespace {

std::string fixture(const char* name) {
    return std::string(ENGINE_TEST_FIXTURE_DIR) + "/audio/" + name;
}

// The services that bind `audio` to a script engine.
ScriptServices with_audio(AudioEngine& audio) {
    ScriptServices services;
    services.audio = &audio;
    return services;
}

}  // namespace

TEST_CASE("A script loads a clip, plays it, and stops it", "[unit]") {
    auto audio = AudioEngine::create_offline();
    ScriptEngine script;
    script.bind(with_audio(*audio));
    const std::string source = R"(
        local clip = audio.load(")" + fixture("tone_mono_s16.wav") + R"(")
        local voice = audio.play(clip, { volume = 0.5, bus = "sfx" })
        assert(audio.is_playing(voice), "voice should be playing")
        assert(audio.stop(voice), "stop should report the voice")
        assert(not audio.is_playing(voice), "voice should be gone")
        assert(not audio.stop(voice), "a second stop finds nothing")
    )";
    auto result = script.run("audio_basic", source);
    INFO("script error: " << (result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());
    CHECK(audio->voice_count() == 0);
    CHECK(script.stack_depth() == 0);
}

TEST_CASE("Spatial voices take a position, move, and listen from the listener", "[unit]") {
    auto audio = AudioEngine::create_offline();
    ScriptEngine script;
    script.bind(with_audio(*audio));
    const std::string source = R"(
        local clip = audio.load(")" + fixture("tone_mono_s16.wav") + R"(")
        audio.set_listener(0, 0, 0, 0, 0, -1, 0, 1, 0)
        local voice = audio.play_at(clip, 5, 0, 0, { min = 1, max = 20, loop = true, pitch = 1.5 })
        assert(audio.set_position(voice, 0, 0, 10), "the voice should still exist")
        assert(audio.is_playing(voice), "a looping voice keeps playing")
    )";
    auto result = script.run("audio_spatial", source);
    INFO("script error: " << (result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());
    CHECK(audio->voice_count() == 1);
}

TEST_CASE("Pause, bus volume, and music are controlled from Lua", "[unit]") {
    auto audio = AudioEngine::create_offline();
    ScriptEngine script;
    script.bind(with_audio(*audio));
    const std::string source = R"(
        audio.pause(true)
        audio.set_bus_volume("music", 0.25)
        audio.set_bus_volume("master", 0.5)
        audio.play_music(")" + fixture("tone_stereo.ogg") + R"(", { loop = false, volume = 0.5 })
        audio.stop_music()
        audio.pause(false)
    )";
    auto result = script.run("audio_mix", source);
    INFO("script error: " << (result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());
    CHECK_FALSE(audio->paused());
    CHECK(audio->bus_volume(engine::audio::Bus::Music) == 0.25f);
    CHECK(audio->bus_volume(engine::audio::Bus::Master) == 0.5f);
    CHECK_FALSE(audio->music_playing());
}

TEST_CASE("Audio errors reach Lua as controlled errors that name the call", "[unit]") {
    auto audio = AudioEngine::create_offline();
    ScriptEngine script;
    script.bind(with_audio(*audio));

    auto unknown = script.run("audio_unknown", "audio.play(99)");
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().message.find("audio.play") != std::string::npos);
    CHECK(unknown.error().message.find("unknown clip id") != std::string::npos);

    auto bus = script.run("audio_bus", "audio.play(1, { bus = 'master' })");
    REQUIRE_FALSE(bus.ok());
    CHECK(bus.error().message.find("bus must be") != std::string::npos);

    auto missing = script.run("audio_missing", "audio.load('" + fixture("missing.wav") + "')");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.find("audio.load") != std::string::npos);

    auto bad_volume = script.run("audio_volume", "audio.set_bus_volume('sfx', -1)");
    REQUIRE_FALSE(bad_volume.ok());

    CHECK(script.stack_depth() == 0);
}

TEST_CASE("Without a bound audio engine the module raises a controlled error", "[unit]") {
    ScriptEngine script;
    auto result = script.run("audio_unbound", "audio.pause(true)");
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().message.find("no audio engine is bound") != std::string::npos);
}

TEST_CASE("A script's voices are released when its ScriptEngine goes away", "[unit]") {
    auto audio = AudioEngine::create_offline();
    VoiceDesc unowned;
    auto clip = audio->load_clip(engine::core::Path::from_utf8(fixture("tone_mono_s16.wav")).value());
    REQUIRE(clip.ok());
    REQUIRE(audio->play(clip.value(), unowned).ok());

    {
        auto script = std::make_unique<ScriptEngine>();
        script->bind(with_audio(*audio));
        auto result = script->run("audio_owned", "audio.play(" + std::to_string(clip.value()) + ", { loop = true })");
        REQUIRE(result.ok());
        CHECK(audio->voice_count() == 2);
    }
    // The unowned voice stays. The script's voice does not outlive the script.
    CHECK(audio->voice_count() == 1);
}
