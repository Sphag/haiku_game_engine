#include "engine/core/file_io.hpp"

#include "engine/core/utf8.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace engine::core {
namespace {

IoError make_error(IoErrorCode code, const Path& path) {
    return IoError{code, path.str()};
}

IoErrorCode classify(const std::error_code& ec) {
    if (ec == std::errc::no_such_file_or_directory) {
        return IoErrorCode::NotFound;
    }
    if (ec == std::errc::permission_denied) {
        return IoErrorCode::AccessDenied;
    }
    if (ec == std::errc::is_a_directory) {
        return IoErrorCode::IsDirectory;
    }
    if (ec == std::errc::not_a_directory) {
        return IoErrorCode::NotDirectory;
    }
    if (ec == std::errc::file_exists) {
        return IoErrorCode::AlreadyExists;
    }
    return IoErrorCode::Other;
}

IoError from_error_code(const std::error_code& ec, const Path& path) {
    return make_error(classify(ec), path);
}

// An open that fails without an error_code: a missing parent is NotFound, anything else is treated as denied.
IoError open_failure(const Path& path, const std::filesystem::path& native) {
    std::error_code ec;
    const auto parent = native.parent_path();
    if (!parent.empty() && !std::filesystem::is_directory(parent, ec)) {
        return make_error(IoErrorCode::NotFound, path);
    }
    return make_error(IoErrorCode::AccessDenied, path);
}

}  // namespace

std::string_view to_string(IoErrorCode code) noexcept {
    switch (code) {
        case IoErrorCode::NotFound:
            return "NotFound";
        case IoErrorCode::AccessDenied:
            return "AccessDenied";
        case IoErrorCode::IsDirectory:
            return "IsDirectory";
        case IoErrorCode::NotDirectory:
            return "NotDirectory";
        case IoErrorCode::AlreadyExists:
            return "AlreadyExists";
        case IoErrorCode::InvalidEncoding:
            return "InvalidEncoding";
        case IoErrorCode::Other:
            return "Other";
    }
    return "Unknown";
}

std::string describe(const IoError& error) {
    return std::string(to_string(error.code)) + ": " + error.path;
}

bool path_exists(const Path& path) {
    std::error_code ec;
    return std::filesystem::exists(to_native_path(path), ec) && !ec;
}

IoResult<std::string> read_file(const Path& path) {
    const auto native = to_native_path(path);
    std::error_code ec;
    const auto info = std::filesystem::status(native, ec);
    if (info.type() == std::filesystem::file_type::not_found) {
        return failure(make_error(IoErrorCode::NotFound, path));
    }
    if (ec) {
        return failure(from_error_code(ec, path));
    }
    if (std::filesystem::is_directory(info)) {
        return failure(make_error(IoErrorCode::IsDirectory, path));
    }

    std::ifstream in(native, std::ios::binary);
    if (!in) {
        return failure(open_failure(path, native));
    }
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return failure(make_error(IoErrorCode::Other, path));
    }
    return success(std::move(contents));
}

IoResult<std::string> read_text_file(const Path& path) {
    auto contents = read_file(path);
    if (!contents.ok()) {
        return contents;
    }
    if (!is_valid_utf8(contents.value())) {
        return failure(make_error(IoErrorCode::InvalidEncoding, path));
    }
    return contents;
}

IoResult<Unit> write_file_atomic(const Path& path, std::string_view contents) {
    const auto target = to_native_path(path);
    auto temporary = target;
    temporary += ".tmp";

    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return failure(open_failure(path, temporary));
        }
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.close();
        if (!out) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return failure(make_error(IoErrorCode::Other, path));
        }
    }

    // std::filesystem::rename replaces an existing file on both platforms.
    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return failure(from_error_code(ec, path));
    }
    return success();
}

IoResult<Unit> create_directories(const Path& path) {
    // Standard libraries disagree on the error for a non-directory in the way (ENOTDIR vs EEXIST),
    // so an existing non-directory is classified here rather than by the error_code.
    const auto native = to_native_path(path);
    std::error_code status_ec;
    const auto status = std::filesystem::status(native, status_ec);
    if (!status_ec && std::filesystem::exists(status) && !std::filesystem::is_directory(status)) {
        return failure(make_error(IoErrorCode::AlreadyExists, path));
    }

    std::error_code ec;
    std::filesystem::create_directories(native, ec);
    if (ec) {
        return failure(from_error_code(ec, path));
    }
    return success();
}

IoResult<Unit> remove_all(const Path& path) {
    std::error_code ec;
    std::filesystem::remove_all(to_native_path(path), ec);
    if (ec) {
        return failure(from_error_code(ec, path));
    }
    return success();
}

}  // namespace engine::core
