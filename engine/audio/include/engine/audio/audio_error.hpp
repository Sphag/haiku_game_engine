#pragma once

// Errors from the audio runtime (US-057). Decoding names the unsupported subtype in the message (for example, the WAV format tag or the
// channel count), so an importer can report exactly what it refused.

#include <string>

namespace engine::audio {

enum class AudioErrorKind {
    FileUnreadable,     // the file could not be read
    UnsupportedFormat,  // a valid container with a subtype the runtime does not decode
    Corrupt,            // the bytes are not a valid container or stream
    Truncated,          // the container declares more bytes than the file holds
    DeviceUnavailable,  // no playback device, or the backend refused to start
    InvalidArgument,    // a clip id, voice id, bus name, or option the caller passed is out of range
};

struct AudioError {
    AudioErrorKind kind = AudioErrorKind::Corrupt;
    std::string message;
};

}  // namespace engine::audio
