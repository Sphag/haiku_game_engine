# Audio fixtures

Synthetic sine tones generated for the US-057 audio tests. No third-party audio is included, so the license is the repository's.

Generated with ffmpeg (lavfi `sine` source, libvorbis encoder for the `.ogg` files):

| File | Command | Format |
|---|---|---|
| `tone_mono_s16.wav` | `-f lavfi -i sine=frequency=440:duration=0.25:sample_rate=48000 -ac 1 -c:a pcm_s16le` | PCM 16-bit, mono, 48 kHz |
| `tone_stereo_f32.wav` | `-f lavfi -i sine=frequency=660:duration=0.25:sample_rate=44100 -ac 2 -c:a pcm_f32le` | IEEE float 32-bit, stereo, 44.1 kHz |
| `tone_mono.ogg` | `-f lavfi -i sine=frequency=440:duration=0.25:sample_rate=22050 -ac 1 -c:a libvorbis -q:a 4` | Ogg Vorbis, mono, 22.05 kHz |
| `tone_stereo.ogg` | `-f lavfi -i sine=frequency=330:duration=0.5:sample_rate=44100 -ac 2 -c:a libvorbis -q:a 4` | Ogg Vorbis, stereo, 44.1 kHz |
