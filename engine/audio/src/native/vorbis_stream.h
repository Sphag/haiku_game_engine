#ifndef ENGINE_AUDIO_NATIVE_VORBIS_STREAM_H
#define ENGINE_AUDIO_NATIVE_VORBIS_STREAM_H

// Ogg Vorbis decoding through stb_vorbis (US-057). stb_vorbis.c is compiled in vorbis_stream.c, outside the first-party warning policy.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct engine_vorbis_stream engine_vorbis_stream;

// Opens the Ogg stream in `data`. The bytes must stay alive and in place until the stream is closed. Returns NULL when the bytes are not a
// valid Vorbis stream. On success it reports the channel count, sample rate, and the total frame count when the decoder knows it.
engine_vorbis_stream* engine_vorbis_open(const uint8_t* data, size_t size, uint32_t* channels, uint32_t* sample_rate,
                                         uint64_t* total_frames);

// Decodes up to `frames` interleaved frames into `out` (frames * channels floats). Returns the frames decoded; 0 at the end or on error.
size_t engine_vorbis_read(engine_vorbis_stream* stream, float* out, size_t frames);

// Moves back to the first frame.
void engine_vorbis_seek_start(engine_vorbis_stream* stream);

void engine_vorbis_close(engine_vorbis_stream* stream);

#ifdef __cplusplus
}
#endif

#endif
