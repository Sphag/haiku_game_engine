#include "engine/core/path.hpp"

#include "engine/core/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace engine::core {
namespace {

// Extended-length prefixes are used from this length, below MAX_PATH (260) with room for file names.
constexpr std::size_t kExtendedLengthThreshold = 240;

bool is_ascii_letter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// "C:/" at the start of a forward-slash path.
bool starts_with_drive_root(std::string_view text) {
    return text.size() >= 3 && is_ascii_letter(text[0]) && text[1] == ':' && text[2] == '/';
}

bool starts_with_unc_root(std::string_view text) {
    return text.size() >= 2 && text[0] == '/' && text[1] == '/';
}

}  // namespace

std::string_view to_string(PathError error) noexcept {
    switch (error) {
        case PathError::InvalidUtf8:
            return "InvalidUtf8";
        case PathError::EmbeddedNul:
            return "EmbeddedNul";
        case PathError::EscapesRoot:
            return "EscapesRoot";
    }
    return "Unknown";
}

Result<Path, PathError> Path::from_utf8(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) {
        return failure(PathError::EmbeddedNul);
    }
    if (!is_valid_utf8(text)) {
        return failure(PathError::InvalidUtf8);
    }

    std::string slashed(text);
    std::replace(slashed.begin(), slashed.end(), '\\', '/');
    std::string_view rest = slashed;

    std::string root;
    if (starts_with_drive_root(rest)) {
        root = std::string(rest.substr(0, 3));
        rest.remove_prefix(3);
    } else if (starts_with_unc_root(rest)) {
        root = "//";
        rest.remove_prefix(2);
    } else if (!rest.empty() && rest.front() == '/') {
        root = "/";
        rest.remove_prefix(1);
    }
    const bool absolute = !root.empty();

    std::vector<std::string_view> parts;
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const auto part = rest.substr(0, slash);
        rest = (slash == std::string_view::npos) ? std::string_view{} : rest.substr(slash + 1);

        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
                continue;
            }
            if (absolute) {
                return failure(PathError::EscapesRoot);
            }
        }
        parts.push_back(part);
    }

    std::string normalized = root;
    for (const auto part : parts) {
        if (!normalized.empty() && normalized.back() != '/') {
            normalized += '/';
        }
        normalized += part;
    }
    if (normalized.empty()) {
        normalized = ".";
    }
    return success(Path(std::move(normalized)));
}

Result<Path, PathError> Path::join(std::string_view component) const {
    std::string combined = text_;
    if (combined.back() != '/') {
        combined += '/';
    }
    combined += component;
    return from_utf8(combined);
}

bool Path::is_absolute() const noexcept {
    return !text_.empty() && (text_.front() == '/' || starts_with_drive_root(text_));
}

std::string_view Path::file_name() const noexcept {
    const auto slash = text_.rfind('/');
    if (slash == std::string::npos) {
        return text_;
    }
    return std::string_view(text_).substr(slash + 1);
}

std::string extended_length_path_text(std::string_view normalized) {
    std::string text(normalized);
    std::replace(text.begin(), text.end(), '/', '\\');

    if (text.size() < kExtendedLengthThreshold || !(starts_with_drive_root(normalized) || starts_with_unc_root(normalized))) {
        return text;
    }
    if (starts_with_unc_root(normalized)) {
        return "\\\\?\\UNC\\" + text.substr(2);
    }
    return "\\\\?\\" + text;
}

std::filesystem::path to_native_path(const Path& path) {
#ifdef _WIN32
    const std::string text = extended_length_path_text(path.str());
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#else
    return std::filesystem::path(path.str());
#endif
}

}  // namespace engine::core
