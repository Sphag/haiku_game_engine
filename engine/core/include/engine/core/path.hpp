#pragma once

#include "engine/core/result.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace engine::core {

enum class PathError {
    InvalidUtf8,
    EmbeddedNul,
    EscapesRoot,
};

[[nodiscard]] std::string_view to_string(PathError error) noexcept;

// A UTF-8 path normalized to forward slashes. Normalization is lexical and never touches the file system:
//   - '\' becomes '/'; repeated separators, '.' components, and trailing separators are removed;
//   - '..' removes the previous component; an absolute path cannot climb above its root (EscapesRoot);
//   - roots are kept: "/" (POSIX), "C:/" (drive), and "//server/share" (UNC);
//   - the caller's UTF-8 bytes are preserved. Unicode normalization (NFC/NFD) is not applied.
class Path {
public:
    [[nodiscard]] static Result<Path, PathError> from_utf8(std::string_view text);

    [[nodiscard]] Result<Path, PathError> join(std::string_view component) const;

    [[nodiscard]] const std::string& str() const noexcept {
        return text_;
    }
    [[nodiscard]] bool is_absolute() const noexcept;
    [[nodiscard]] std::string_view file_name() const noexcept;

    friend bool operator==(const Path& lhs, const Path& rhs) noexcept {
        return lhs.text_ == rhs.text_;
    }

private:
    explicit Path(std::string text) : text_(std::move(text)) {}

    std::string text_;
};

// Windows file APIs fail on absolute paths of MAX_PATH (260) characters or more unless they use the
// extended-length form. This transformation is pure, so it is tested on every platform: it converts '/' to '\'
// and prefixes absolute paths of 240 or more characters with "\\?\" (or "\\?\UNC\" for UNC paths).
// Relative paths are returned without a prefix; they are resolved by the caller's working directory.
[[nodiscard]] std::string extended_length_path_text(std::string_view normalized);

// The path in the form the platform file APIs accept: UTF-8 on POSIX; UTF-16 with the extended-length prefix
// when needed on Windows.
[[nodiscard]] std::filesystem::path to_native_path(const Path& path);

}  // namespace engine::core
