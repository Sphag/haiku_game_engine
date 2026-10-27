// stb_vorbis backs the Ogg Vorbis decoder. The implementation is vendored unchanged; this file only wraps it in the streaming calls.

#include "vorbis_stream.h"

#include <limits.h>
#include <stdlib.h>

#include "stb/stb_vorbis.c"

struct engine_vorbis_stream {
    stb_vorbis* decoder;
    uint32_t channels;
};

engine_vorbis_stream* engine_vorbis_open(const uint8_t* data, size_t size, uint32_t* channels, uint32_t* sample_rate,
                                         uint64_t* total_frames) {
    engine_vorbis_stream* stream;
    stb_vorbis* decoder;
    stb_vorbis_info info;
    int error = 0;

    if (size == 0 || size > (size_t)INT_MAX) {
        return NULL;
    }
    decoder = stb_vorbis_open_memory(data, (int)size, &error, NULL);
    if (decoder == NULL) {
        return NULL;
    }
    info = stb_vorbis_get_info(decoder);
    if (info.channels <= 0 || info.sample_rate == 0) {
        stb_vorbis_close(decoder);
        return NULL;
    }
    stream = (engine_vorbis_stream*)malloc(sizeof(*stream));
    if (stream == NULL) {
        stb_vorbis_close(decoder);
        return NULL;
    }
    stream->decoder = decoder;
    stream->channels = (uint32_t)info.channels;
    *channels = stream->channels;
    *sample_rate = info.sample_rate;
    *total_frames = stb_vorbis_stream_length_in_samples(decoder);
    return stream;
}

size_t engine_vorbis_read(engine_vorbis_stream* stream, float* out, size_t frames) {
    int decoded;
    if (stream == NULL || frames == 0) {
        return 0;
    }
    if (frames > (size_t)(INT_MAX / stream->channels)) {
        frames = (size_t)(INT_MAX / stream->channels);
    }
    decoded = stb_vorbis_get_samples_float_interleaved(stream->decoder, (int)stream->channels, out, (int)(frames * stream->channels));
    return decoded > 0 ? (size_t)decoded : 0;
}

void engine_vorbis_seek_start(engine_vorbis_stream* stream) {
    if (stream != NULL) {
        stb_vorbis_seek_start(stream->decoder);
    }
}

void engine_vorbis_close(engine_vorbis_stream* stream) {
    if (stream == NULL) {
        return;
    }
    stb_vorbis_close(stream->decoder);
    free(stream);
}
