#pragma once

// The audio engine (US-057): clips, voices, music, buses, and the listener, behind one lock. A device renders the mixer from its own
// thread; without a device the engine still runs, and render() drives it for offline use. The mixer never reads ECS state. The owner
// sets voice positions between frames, and the device thread reads only the mixer's copies.
//
// Pause and world unload: set_paused silences every voice and stream without releasing it. stop_owner releases the voices a world or a
// script started, so unloading a world does not leave its sounds playing.

#include "engine/audio/audio_clip.hpp"
#include "engine/audio/audio_error.hpp"
#include "engine/audio/mixer.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include "engine/audio/byte_view.hpp"
#include <string>
#include <vector>

namespace engine::audio {

class AudioDevice;

using ClipId = std::uint32_t;  // 0 is never a clip

class AudioEngine {
public:
    static constexpr std::uint32_t kDefaultSampleRate = 48000;

    // An engine with no device. Call render() to produce its output.
    [[nodiscard]] static std::unique_ptr<AudioEngine> create_offline(std::uint32_t sample_rate = kDefaultSampleRate);
    // An engine that plays through the default device. Fails with DeviceUnavailable when the system has no usable output; the offline
    // engine is the fallback for that case.
    [[nodiscard]] static core::Result<std::unique_ptr<AudioEngine>, AudioError> open_device(std::uint32_t sample_rate = kDefaultSampleRate);

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&) = delete;
    AudioEngine& operator=(AudioEngine&&) = delete;
    ~AudioEngine();

    [[nodiscard]] bool has_device() const noexcept {
        return device_ != nullptr;
    }
    [[nodiscard]] std::string device_name() const;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept {
        return sample_rate_;
    }

    // Clips stay loaded until the engine is destroyed. Voices hold their own reference, so a clip is never freed under a playing voice.
    [[nodiscard]] core::Result<ClipId, AudioError> load_clip(const core::Path& path);
    [[nodiscard]] core::Result<ClipId, AudioError> add_clip(AudioClip clip);

    [[nodiscard]] core::Result<VoiceId, AudioError> play(ClipId clip, const VoiceDesc& desc);
    bool stop(VoiceId voice);
    std::size_t stop_owner(OwnerId owner);
    void stop_all();
    bool set_voice_position(VoiceId voice, glm::dvec3 position);
    [[nodiscard]] bool is_playing(VoiceId voice);
    [[nodiscard]] std::size_t voice_count();

    void set_listener(const Listener& listener);
    void set_bus_volume(Bus bus, float volume);
    [[nodiscard]] float bus_volume(Bus bus);
    void set_paused(bool paused);
    [[nodiscard]] bool paused();

    [[nodiscard]] core::Result<core::Unit, AudioError> play_music_file(const core::Path& path, const MusicDesc& desc);
    [[nodiscard]] core::Result<core::Unit, AudioError> play_music(ByteView bytes, const MusicDesc& desc);
    void stop_music();
    [[nodiscard]] bool music_playing();

    // Renders `frames` stereo frames into `out`. The device calls this from its thread; offline callers call it directly.
    void render(float* out, std::size_t frames);

private:
    explicit AudioEngine(std::uint32_t sample_rate);
    [[nodiscard]] const AudioClip* clip_at(ClipId clip) const;

    std::uint32_t sample_rate_;
    std::mutex mutex_;
    Mixer mixer_;
    std::vector<std::shared_ptr<const AudioClip>> clips_;  // index is ClipId - 1
    std::unique_ptr<AudioDevice> device_;
};

}  // namespace engine::audio
