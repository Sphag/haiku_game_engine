#pragma once

#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <string>
#include <string_view>

namespace engine::core {

enum class IoErrorCode {
    NotFound,
    AccessDenied,
    IsDirectory,
    NotDirectory,
    AlreadyExists,
    InvalidEncoding,
    Other,
};

[[nodiscard]] std::string_view to_string(IoErrorCode code) noexcept;

struct IoError {
    IoErrorCode code;
    std::string path;  // UTF-8 path that the failed operation used
};

// "NotFound: <path>", for logs and diagnostics.
[[nodiscard]] std::string describe(const IoError& error);

template <typename T>
using IoResult = Result<T, IoError>;

[[nodiscard]] bool path_exists(const Path& path);

// Returns the file's bytes unchanged.
[[nodiscard]] IoResult<std::string> read_file(const Path& path);

// Like read_file, but fails with InvalidEncoding unless the contents are valid UTF-8.
[[nodiscard]] IoResult<std::string> read_text_file(const Path& path);

// Replaces the file atomically: writes "<path>.tmp" in the same directory, then renames it over the target.
// The parent directory must exist (NotFound otherwise). A failed write leaves the previous file in place.
[[nodiscard]] IoResult<Unit> write_file_atomic(const Path& path, std::string_view contents);

// Creates the directory and any missing parents. An existing directory is not an error.
[[nodiscard]] IoResult<Unit> create_directories(const Path& path);

// Removes a file or a whole tree. A missing path is not an error.
[[nodiscard]] IoResult<Unit> remove_all(const Path& path);

}  // namespace engine::core
