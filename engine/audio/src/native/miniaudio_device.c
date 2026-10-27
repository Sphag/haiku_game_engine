// miniaudio backs the playback device. This file is the only translation unit that includes miniaudio.h; its implementation sits
// outside the first-party warning policy, as the Lua and stb sources do.

#include "miniaudio_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENGINE
#include "miniaudio.h"

struct engine_audio_device {
    ma_device device;
    engine_audio_pull_fn pull;
    void* user;
    int initialized;
};

static void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    engine_audio_device* self = (engine_audio_device*)device->pUserData;
    (void)input;
    self->pull(self->user, (float*)output, (uint32_t)frame_count);
}

int engine_audio_device_open(uint32_t sample_rate, engine_audio_pull_fn pull, void* user, engine_audio_device** out_device,
                             char* message, size_t message_size) {
    ma_device_config config;
    engine_audio_device* self;
    ma_result result;

    *out_device = NULL;
    self = (engine_audio_device*)calloc(1, sizeof(*self));
    if (self == NULL) {
        if (message != NULL && message_size > 0) {
            snprintf(message, message_size, "out of memory");
        }
        return (int)MA_OUT_OF_MEMORY;
    }
    self->pull = pull;
    self->user = user;

    config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = sample_rate;
    config.dataCallback = data_callback;
    config.pUserData = self;

    result = ma_device_init(NULL, &config, &self->device);
    if (result != MA_SUCCESS) {
        if (message != NULL && message_size > 0) {
            snprintf(message, message_size, "%s", ma_result_description(result));
        }
        free(self);
        return (int)result;
    }
    self->initialized = 1;
    *out_device = self;
    return 0;
}

int engine_audio_device_start(engine_audio_device* device) {
    return device == NULL ? (int)MA_INVALID_ARGS : (int)ma_device_start(&device->device);
}

int engine_audio_device_stop(engine_audio_device* device) {
    return device == NULL ? (int)MA_INVALID_ARGS : (int)ma_device_stop(&device->device);
}

uint32_t engine_audio_device_sample_rate(const engine_audio_device* device) {
    return device == NULL ? 0u : (uint32_t)device->device.sampleRate;
}

void engine_audio_device_name(const engine_audio_device* device, char* name, size_t name_size) {
    if (name == NULL || name_size == 0) {
        return;
    }
    if (device == NULL) {
        name[0] = '\0';
        return;
    }
    snprintf(name, name_size, "%s", device->device.playback.name);
}

void engine_audio_device_close(engine_audio_device* device) {
    if (device == NULL) {
        return;
    }
    if (device->initialized) {
        ma_device_uninit(&device->device);
    }
    free(device);
}
