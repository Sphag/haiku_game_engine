#ifndef ENGINE_AUDIO_NATIVE_MINIAUDIO_DEVICE_H
#define ENGINE_AUDIO_NATIVE_MINIAUDIO_DEVICE_H

// The playback device behind the audio engine (US-057). miniaudio is compiled in miniaudio_device.c, so its header never reaches the
// first-party C++ translation units. The device pulls interleaved stereo float frames from the callback on its own thread.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*engine_audio_pull_fn)(void* user, float* out_interleaved_stereo, uint32_t frames);

typedef struct engine_audio_device engine_audio_device;

// Opens the default playback device at `sample_rate` (stereo, f32). Returns 0 on success. Otherwise it returns the backend's error code
// and writes its description into `message`, so the caller can report why no device is available.
int engine_audio_device_open(uint32_t sample_rate, engine_audio_pull_fn pull, void* user, engine_audio_device** out_device,
                             char* message, size_t message_size);

// Starts or stops the callbacks. Both return 0 on success.
int engine_audio_device_start(engine_audio_device* device);
int engine_audio_device_stop(engine_audio_device* device);

// The rate the device actually runs at, which can differ from the one requested. Zero for a null handle.
uint32_t engine_audio_device_sample_rate(const engine_audio_device* device);

// Writes the device name into `name` (NUL-terminated, truncated to `name_size`).
void engine_audio_device_name(const engine_audio_device* device, char* name, size_t name_size);

// Stops the callbacks, releases the device, and frees the handle. A null device is ignored.
void engine_audio_device_close(engine_audio_device* device);

#ifdef __cplusplus
}
#endif

#endif
