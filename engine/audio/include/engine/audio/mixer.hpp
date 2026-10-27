#pragma once

// The offline mixer (US-057): voices, buses, listener, spatial attenuation, and streamed music, rendered to stereo float frames. It
// has no device, no thread, and no ECS access, so the tests render it directly. The audio engine serialises calls into it.

#include "engine/audio/audio_clip.hpp"
#include "engine/audio/audio_error.hpp"
#include "engine/audio/music_stream.hpp"
#include "engine/core/result.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace engine::audio {

using VoiceId = std::uint64_t;  // 0 is never a voice; ids are not reused
using OwnerId = std::uint64_t;  // 0 means no owner; a script or world tags the voices it starts so they can be released together

enum class Bus : std::uint8_t {
    Master,  // scales every bus
    Music,
    Sfx,
};

struct VoiceDesc {
    Bus bus = Bus::Sfx;             // Music or Sfx; Master is rejected for a voice
    float volume = 1.0f;            // voice gain, before the bus and master gains
    float pitch = 1.0f;             // playback speed; 2 plays an octave up and half as long
    bool loop = false;              // restart at the first frame when the clip ends
    bool spatial = false;           // true: attenuate by distance from the listener and pan by direction; false: pan by `pan`
    glm::dvec3 position{0.0};       // world position for a spatial voice
    float min_distance = 1.0f;      // full volume inside this distance (metres)
    float max_distance = 50.0f;     // silent at and beyond this distance; must exceed min_distance
    float pan = 0.0f;               // non-spatial: -1 is full left, 1 full right
    OwnerId owner = 0;
};

struct Listener {
    glm::dvec3 position{0.0};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

struct MusicDesc {
    bool loop = true;
    float volume = 1.0f;
};

// Distance attenuation: full gain up to min_distance, min/d between, and silent from max_distance on.
[[nodiscard]] float attenuation(double distance, float min_distance, float max_distance) noexcept;

class Mixer {
public:
    explicit Mixer(std::uint32_t sample_rate);

    [[nodiscard]] std::uint32_t sample_rate() const noexcept {
        return sample_rate_;
    }

    [[nodiscard]] core::Result<VoiceId, AudioError> play(std::shared_ptr<const AudioClip> clip, const VoiceDesc& desc);
    // Returns false when the voice has already finished or was stopped.
    bool stop(VoiceId voice);
    // Stops every voice tagged with `owner`, and returns how many it stopped.
    std::size_t stop_owner(OwnerId owner);
    void stop_all();
    bool set_voice_position(VoiceId voice, glm::dvec3 position);
    [[nodiscard]] bool is_playing(VoiceId voice) const;
    [[nodiscard]] std::size_t voice_count() const noexcept {
        return voices_.size();
    }

    void set_listener(const Listener& listener) noexcept {
        listener_ = listener;
    }
    [[nodiscard]] const Listener& listener() const noexcept {
        return listener_;
    }

    // Bus gains are linear and not clamped; 0 silences the bus.
    void set_bus_volume(Bus bus, float volume) noexcept;
    [[nodiscard]] float bus_volume(Bus bus) const noexcept;

    // While paused, render() outputs silence and no voice or stream advances.
    void set_paused(bool paused) noexcept {
        paused_ = paused;
    }
    [[nodiscard]] bool paused() const noexcept {
        return paused_;
    }

    // One music track at a time; a new track replaces the old one.
    core::Result<core::Unit, AudioError> play_music(MusicStream stream, const MusicDesc& desc);
    void stop_music() noexcept;
    [[nodiscard]] bool music_playing() const noexcept;

    // Writes `frames` stereo frames (2 * frames floats), replacing what the buffer held.
    void render(float* out, std::size_t frames);

private:
    struct Voice {
        VoiceId id = 0;
        std::shared_ptr<const AudioClip> clip;
        VoiceDesc desc;
        double cursor = 0.0;  // in clip frames
        double step = 1.0;    // clip frames per output frame
        bool finished = false;
    };

    struct Music {
        MusicStream stream;
        MusicDesc desc;
        double step = 1.0;  // stream frames per output frame
        double phase = 0.0;  // position between `previous` and `next`, in [0, 1)
        float previous[2] = {0.0f, 0.0f};
        float next[2] = {0.0f, 0.0f};
        bool primed = false;
        bool ended = false;
        bool finished = false;
    };

    void render_voice(Voice& voice, float* out, std::size_t frames);
    void render_music(float* out, std::size_t frames);
    bool read_music_frame(float* frame);
    [[nodiscard]] float bus_gain(Bus bus) const noexcept;

    std::uint32_t sample_rate_;
    VoiceId next_voice_ = 1;
    std::vector<Voice> voices_;
    std::optional<Music> music_;
    Listener listener_;
    float bus_volumes_[3] = {1.0f, 1.0f, 1.0f};
    bool paused_ = false;
};

}  // namespace engine::audio
