#include "engine/audio/mixer.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace engine::audio {
namespace {

constexpr float kPi = 3.14159265358979323846f;

core::Failure<AudioError> invalid(std::string message) {
    return core::failure(AudioError{AudioErrorKind::InvalidArgument, std::move(message)});
}

// Equal-power pan for a mono source: -1 is full left, 0 is centre (each side at -3 dB), 1 is full right.
void mono_pan_gains(float pan, float& left, float& right) noexcept {
    const float angle = (pan + 1.0f) * kPi / 4.0f;
    left = std::cos(angle);
    right = std::sin(angle);
}

// Balance for a stereo source: the side away from the pan direction is attenuated, and nothing is mixed across.
void balance_gains(float pan, float& left, float& right) noexcept {
    left = std::min(1.0f, 1.0f - pan);
    right = std::min(1.0f, 1.0f + pan);
}

}  // namespace

float attenuation(double distance, float min_distance, float max_distance) noexcept {
    if (distance <= min_distance) {
        return 1.0f;
    }
    if (distance >= max_distance) {
        return 0.0f;
    }
    return static_cast<float>(min_distance / distance);
}

Mixer::Mixer(std::uint32_t sample_rate) : sample_rate_(sample_rate) {}

core::Result<VoiceId, AudioError> Mixer::play(std::shared_ptr<const AudioClip> clip, const VoiceDesc& desc) {
    if (!clip || clip->frame_count() == 0 || clip->sample_rate == 0) {
        return invalid("clip has no samples");
    }
    if (desc.bus == Bus::Master) {
        return invalid("voices play on the music or sfx bus, not on master");
    }
    if (!std::isfinite(desc.volume) || desc.volume < 0.0f) {
        return invalid("voice volume must be a finite number, zero or more");
    }
    if (!std::isfinite(desc.pitch) || desc.pitch <= 0.0f) {
        return invalid("voice pitch must be a finite number above zero");
    }
    if (!std::isfinite(desc.pan) || desc.pan < -1.0f || desc.pan > 1.0f) {
        return invalid("voice pan must be between -1 and 1");
    }
    if (desc.spatial && (!(desc.min_distance > 0.0f) || !(desc.max_distance > desc.min_distance))) {
        return invalid("spatial voice needs 0 < min_distance < max_distance");
    }
    Voice voice;
    voice.id = next_voice_++;
    voice.step = static_cast<double>(clip->sample_rate) / static_cast<double>(sample_rate_) * static_cast<double>(desc.pitch);
    voice.clip = std::move(clip);
    voice.desc = desc;
    voices_.push_back(std::move(voice));
    return core::success(voices_.back().id);
}

bool Mixer::stop(VoiceId voice) {
    return std::erase_if(voices_, [voice](const Voice& v) { return v.id == voice; }) > 0;
}

std::size_t Mixer::stop_owner(OwnerId owner) {
    if (owner == 0) {
        return 0;
    }
    return std::erase_if(voices_, [owner](const Voice& v) { return v.desc.owner == owner; });
}

void Mixer::stop_all() {
    voices_.clear();
}

bool Mixer::set_voice_position(VoiceId voice, glm::dvec3 position) {
    for (Voice& v : voices_) {
        if (v.id == voice) {
            v.desc.position = position;
            return true;
        }
    }
    return false;
}

bool Mixer::is_playing(VoiceId voice) const {
    return std::any_of(voices_.begin(), voices_.end(), [voice](const Voice& v) { return v.id == voice; });
}

void Mixer::set_bus_volume(Bus bus, float volume) noexcept {
    bus_volumes_[static_cast<std::size_t>(bus)] = volume;
}

float Mixer::bus_volume(Bus bus) const noexcept {
    return bus_volumes_[static_cast<std::size_t>(bus)];
}

float Mixer::bus_gain(Bus bus) const noexcept {
    return bus_volumes_[static_cast<std::size_t>(bus)];
}

core::Result<core::Unit, AudioError> Mixer::play_music(MusicStream stream, const MusicDesc& desc) {
    if (!std::isfinite(desc.volume) || desc.volume < 0.0f) {
        return invalid("music volume must be a finite number, zero or more");
    }
    const double step = static_cast<double>(stream.sample_rate()) / static_cast<double>(sample_rate_);
    music_ = Music{std::move(stream), desc, step, 0.0, {0.0f, 0.0f}, {0.0f, 0.0f}, false, false, false};
    return core::success(core::Unit{});
}

void Mixer::stop_music() noexcept {
    music_.reset();
}

bool Mixer::music_playing() const noexcept {
    return music_.has_value() && !music_->finished;
}

bool Mixer::read_music_frame(float* frame) {
    Music& music = *music_;
    float samples[2] = {0.0f, 0.0f};
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (music.stream.read(samples, 1) == 1) {
            frame[0] = samples[0];
            frame[1] = music.stream.channels() == 1 ? samples[0] : samples[1];
            return true;
        }
        // The stream ended: loop from the start once, and give up when that is also empty or looping is off.
        if (!music.desc.loop) {
            break;
        }
        music.stream.rewind();
    }
    frame[0] = 0.0f;
    frame[1] = 0.0f;
    return false;
}

void Mixer::render_music(float* out, std::size_t frames) {
    Music& music = *music_;
    const float gain = music.desc.volume * bus_gain(Bus::Music);
    if (!music.primed) {
        music.primed = true;
        music.ended = !read_music_frame(music.previous);
        if (!music.ended) {
            music.ended = !read_music_frame(music.next);
        }
    }
    for (std::size_t f = 0; f < frames && !music.finished; ++f) {
        const float t = static_cast<float>(music.phase);
        out[2 * f] += gain * (music.previous[0] + (music.next[0] - music.previous[0]) * t);
        out[2 * f + 1] += gain * (music.previous[1] + (music.next[1] - music.previous[1]) * t);
        music.phase += music.step;
        while (music.phase >= 1.0) {
            music.phase -= 1.0;
            music.previous[0] = music.next[0];
            music.previous[1] = music.next[1];
            if (music.ended) {
                music.finished = true;
                break;
            }
            if (!read_music_frame(music.next)) {
                music.ended = true;
            }
        }
    }
}

void Mixer::render_voice(Voice& voice, float* out, std::size_t frames) {
    const AudioClip& clip = *voice.clip;
    const std::size_t count = clip.frame_count();
    const std::size_t stride = clip.channels;
    const VoiceDesc& desc = voice.desc;

    float volume = desc.volume * bus_gain(desc.bus);
    float pan = desc.pan;
    if (desc.spatial) {
        const glm::dvec3 offset = desc.position - listener_.position;
        const double distance = glm::length(offset);
        volume *= attenuation(distance, desc.min_distance, desc.max_distance);
        glm::dvec3 right = glm::cross(glm::dvec3(listener_.forward), glm::dvec3(listener_.up));
        if (glm::length(right) < 1e-9) {
            right = glm::dvec3(1.0, 0.0, 0.0);  // a degenerate listener frame still pans to the x axis
        }
        right = glm::normalize(right);
        if (distance > 0.0) {
            pan = static_cast<float>(std::clamp(glm::dot(offset / distance, right), -1.0, 1.0));
        }
    }
    float gain_left = 0.0f;
    float gain_right = 0.0f;
    if (stride == 1) {
        mono_pan_gains(pan, gain_left, gain_right);
    } else {
        balance_gains(pan, gain_left, gain_right);
    }

    for (std::size_t f = 0; f < frames; ++f) {
        if (!desc.loop && voice.cursor >= static_cast<double>(count)) {
            voice.finished = true;
            break;
        }
        const double position = desc.loop ? std::fmod(voice.cursor, static_cast<double>(count)) : voice.cursor;
        const std::size_t i0 = std::min(static_cast<std::size_t>(position), count - 1);
        const float t = static_cast<float>(position - static_cast<double>(i0));
        std::size_t i1 = i0 + 1;
        if (i1 >= count) {
            i1 = desc.loop ? 0 : i0;
        }
        const float* a = clip.samples.data() + i0 * stride;
        const float* b = clip.samples.data() + i1 * stride;
        if (stride == 1) {
            const float sample = (a[0] + (b[0] - a[0]) * t) * volume;
            out[2 * f] += sample * gain_left;
            out[2 * f + 1] += sample * gain_right;
        } else {
            out[2 * f] += (a[0] + (b[0] - a[0]) * t) * volume * gain_left;
            out[2 * f + 1] += (a[1] + (b[1] - a[1]) * t) * volume * gain_right;
        }
        voice.cursor += voice.step;
    }
    if (!desc.loop && voice.cursor >= static_cast<double>(count)) {
        voice.finished = true;
    }
}

void Mixer::render(float* out, std::size_t frames) {
    std::fill(out, out + 2 * frames, 0.0f);
    if (paused_) {
        return;
    }
    for (Voice& voice : voices_) {
        render_voice(voice, out, frames);
    }
    std::erase_if(voices_, [](const Voice& v) { return v.finished; });
    if (music_) {
        render_music(out, frames);
    }
    const float master = bus_gain(Bus::Master);
    for (std::size_t i = 0; i < 2 * frames; ++i) {
        out[i] = std::clamp(out[i] * master, -1.0f, 1.0f);
    }
}

}  // namespace engine::audio
