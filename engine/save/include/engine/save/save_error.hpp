#pragma once

#include <string>

namespace engine::save {

enum class SaveErrorCode {
    Io,                 // a file could not be read or written
    NoSave,             // the slot has no save file
    Malformed,          // not JSON, a missing or unknown key, or a value of the wrong JSON type
    WrongFormat,        // the format name is not "engine.save"
    UnsupportedVersion, // the format version is not this build's
    ChecksumMismatch,   // the body does not match the checksum: the file is corrupt
    InvalidContent,     // the scene, a world cell, a script state, or the world after applying the changes fails its checks
    InvalidSlot,        // the slot name is empty, too long, or has a character outside [A-Za-z0-9_-]
};

struct SaveError {
    SaveErrorCode code;
    std::string message;
};

}  // namespace engine::save
