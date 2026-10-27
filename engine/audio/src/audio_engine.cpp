#include "engine/audio/audio_engine.hpp"

#include "native/miniaudio_device.h"

#include "engine/core/file_io.hpp"

#include <string>
#include <utility>

namespace engine::audio {
namespace {

core::Failure<AudioError> device_error(const char* what, const std::string& detail) {
    return core::failure(AudioError{AudioErrorKind::DeviceUnavailable, std::string(what) + ": " + detail});
}

}  // namespace

// The device handle and its callback. The C side calls pull() on the device thread; the engine locks its mutex in render().
class AudioDevice {
public:
    struct Closer {
        void operator()(engine_audio_device* device) const noexcept {
            engine_audio_device_close(device);
        }
    };

    static void pull(void* user, float* out, std::uint32_t frames) {
        static_cast<AudioEngine*>(user)->render(out, frames);
    }

    // Closing the handle stops the callbacks, so the device must go before the mixer it renders.
    std::unique_ptr<engine_audio_device, Closer> handle;
};

AudioEngine::AudioEngine(std::uint32_t sample_rate) : sample_rate_(sample_rate), mixer_(sample_rate) {}

AudioEngine::~AudioEngine() {
    // Closing the device stops the callbacks before any member is destroyed.
    device_.reset();
}

std::unique_ptr<AudioEngine> AudioEngine::create_offline(std::uint32_t sample_rate) {
    return std::unique_ptr<AudioEngine>(new AudioEngine(sample_rate));
}

core::Result<std::unique_ptr<AudioEngine>, AudioError> AudioEngine::open_device(std::uint32_t sample_rate) {
    std::unique_ptr<AudioEngine> engine(new AudioEngine(sample_rate));
    char message[256] = {};
    engine_audio_device* handle = nullptr;
    const int code = engine_audio_device_open(sample_rate, &AudioDevice::pull, engine.get(), &handle, message, sizeof(message));
    if (code != 0) {
        return device_error("no playback device", message[0] != '\0' ? std::string(message) : "code " + std::to_string(code));
    }
    // The device owns the handle from here on, so a failed start closes it.
    auto device = std::make_unique<AudioDevice>();
    device->handle.reset(handle);
    // The mixer must render at the rate the device runs at. No callback has started yet, so the mixer can be rebuilt here.
    const std::uint32_t actual_rate = engine_audio_device_sample_rate(handle);
    if (actual_rate != 0 && actual_rate != sample_rate) {
        engine->sample_rate_ = actual_rate;
        engine->mixer_ = Mixer(actual_rate);
    }
    const int started = engine_audio_device_start(handle);
    if (started != 0) {
        return device_error("playback device would not start", "code " + std::to_string(started));
    }
    engine->device_ = std::move(device);
    return core::success(std::move(engine));
}

std::string AudioEngine::device_name() const {
    if (!device_) {
        return {};
    }
    char name[256] = {};
    engine_audio_device_name(device_->handle.get(), name, sizeof(name));
    return name;
}

const AudioClip* AudioEngine::clip_at(ClipId clip) const {
    if (clip == 0 || clip > clips_.size()) {
        return nullptr;
    }
    return clips_[clip - 1].get();
}

core::Result<ClipId, AudioError> AudioEngine::load_clip(const core::Path& path) {
    auto decoded = load_audio_clip(path);
    if (!decoded.ok()) {
        return core::failure(decoded.error());
    }
    return add_clip(std::move(decoded.value()));
}

core::Result<ClipId, AudioError> AudioEngine::add_clip(AudioClip clip) {
    if (clip.frame_count() == 0 || (clip.channels != 1 && clip.channels != 2) || clip.sample_rate == 0) {
        return core::failure(AudioError{AudioErrorKind::InvalidArgument, "clip must have mono or stereo frames and a sample rate"});
    }
    std::lock_guard lock(mutex_);
    clips_.push_back(std::make_shared<const AudioClip>(std::move(clip)));
    return core::success(static_cast<ClipId>(clips_.size()));
}

core::Result<VoiceId, AudioError> AudioEngine::play(ClipId clip, const VoiceDesc& desc) {
    std::lock_guard lock(mutex_);
    const AudioClip* found = clip_at(clip);
    if (found == nullptr) {
        return core::failure(AudioError{AudioErrorKind::InvalidArgument, "unknown clip id " + std::to_string(clip)});
    }
    return mixer_.play(clips_[clip - 1], desc);
}

bool AudioEngine::stop(VoiceId voice) {
    std::lock_guard lock(mutex_);
    return mixer_.stop(voice);
}

std::size_t AudioEngine::stop_owner(OwnerId owner) {
    std::lock_guard lock(mutex_);
    return mixer_.stop_owner(owner);
}

void AudioEngine::stop_all() {
    std::lock_guard lock(mutex_);
    mixer_.stop_all();
}

bool AudioEngine::set_voice_position(VoiceId voice, glm::dvec3 position) {
    std::lock_guard lock(mutex_);
    return mixer_.set_voice_position(voice, position);
}

bool AudioEngine::is_playing(VoiceId voice) {
    std::lock_guard lock(mutex_);
    return mixer_.is_playing(voice);
}

std::size_t AudioEngine::voice_count() {
    std::lock_guard lock(mutex_);
    return mixer_.voice_count();
}

void AudioEngine::set_listener(const Listener& listener) {
    std::lock_guard lock(mutex_);
    mixer_.set_listener(listener);
}

void AudioEngine::set_bus_volume(Bus bus, float volume) {
    std::lock_guard lock(mutex_);
    mixer_.set_bus_volume(bus, volume);
}

float AudioEngine::bus_volume(Bus bus) {
    std::lock_guard lock(mutex_);
    return mixer_.bus_volume(bus);
}

void AudioEngine::set_paused(bool paused) {
    std::lock_guard lock(mutex_);
    mixer_.set_paused(paused);
}

bool AudioEngine::paused() {
    std::lock_guard lock(mutex_);
    return mixer_.paused();
}

core::Result<core::Unit, AudioError> AudioEngine::play_music_file(const core::Path& path, const MusicDesc& desc) {
    auto file = core::read_file(path);
    if (!file.ok()) {
        return core::failure(AudioError{AudioErrorKind::FileUnreadable, core::describe(file.error())});
    }
    const std::string& text = file.value();
    return play_music(ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), desc);
}

core::Result<core::Unit, AudioError> AudioEngine::play_music(ByteView bytes, const MusicDesc& desc) {
    auto stream = MusicStream::open(bytes);
    if (!stream.ok()) {
        return core::failure(stream.error());
    }
    std::lock_guard lock(mutex_);
    return mixer_.play_music(std::move(stream.value()), desc);
}

void AudioEngine::stop_music() {
    std::lock_guard lock(mutex_);
    mixer_.stop_music();
}

bool AudioEngine::music_playing() {
    std::lock_guard lock(mutex_);
    return mixer_.music_playing();
}

void AudioEngine::render(float* out, std::size_t frames) {
    std::lock_guard lock(mutex_);
    mixer_.render(out, frames);
}

}  // namespace engine::audio
