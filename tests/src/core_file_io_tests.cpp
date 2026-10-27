#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using engine::core::create_directories;
using engine::core::describe;
using engine::core::IoErrorCode;
using engine::core::IoResult;
using engine::core::Path;
using engine::core::path_exists;
using engine::core::read_file;
using engine::core::read_text_file;
using engine::core::remove_all;
using engine::core::Unit;
using engine::core::write_file_atomic;

namespace {

Path require_path(std::string_view text) {
    const auto path = Path::from_utf8(text);
    REQUIRE(path.ok());
    return path.value();
}

Path join_path(const Path& base, std::string_view component) {
    const auto joined = base.join(component);
    REQUIRE(joined.ok());
    return joined.value();
}

std::string utf8_text(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// A unique directory under the system temp location, removed when the test ends.
class ScratchDirectory {
public:
    ScratchDirectory() {
        const auto base = require_path(utf8_text(std::filesystem::temp_directory_path()));
        path_ = join_path(base, "engine_core_tests_" + engine::core::Guid::generate().to_string());
        REQUIRE(create_directories(path_).ok());
    }
    ~ScratchDirectory() {
        static_cast<void>(remove_all(path_));
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    [[nodiscard]] const Path& path() const {
        return path_;
    }

private:
    Path path_ = require_path(".");
};

}  // namespace

TEST_CASE("Cyrillic file names and contents round-trip through atomic write and read", "[integration]") {
    const ScratchDirectory scratch;
    const auto folder = join_path(scratch.path(), "папка с пробелом");
    REQUIRE(create_directories(folder).ok());
    const auto file = join_path(folder, "Привет, мир.txt");

    REQUIRE(write_file_atomic(file, "Привет, мир!\n").ok());

    const auto contents = read_text_file(file);
    REQUIRE(contents.ok());
    CHECK(contents.value() == "Привет, мир!\n");
    CHECK(file.file_name() == "Привет, мир.txt");
}

TEST_CASE("Atomic write replaces the previous contents and leaves no temporary file", "[integration]") {
    const ScratchDirectory scratch;
    const auto file = join_path(scratch.path(), "state.txt");

    REQUIRE(write_file_atomic(file, "first").ok());
    REQUIRE(write_file_atomic(file, "second").ok());

    CHECK(read_file(file).value() == "second");
    CHECK_FALSE(path_exists(join_path(scratch.path(), "state.txt.tmp")));
}

TEST_CASE("Reading a missing file reports NotFound with the path", "[integration]") {
    const ScratchDirectory scratch;
    const auto missing = join_path(scratch.path(), "missing.bin");

    const auto result = read_file(missing);

    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == IoErrorCode::NotFound);
    CHECK(result.error().path == missing.str());
    CHECK(describe(result.error()).rfind("NotFound: ", 0) == 0);
}

TEST_CASE("Reading a directory reports IsDirectory", "[integration]") {
    const ScratchDirectory scratch;

    const auto result = read_file(scratch.path());

    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == IoErrorCode::IsDirectory);
}

TEST_CASE("Writing into a missing directory reports NotFound and creates nothing", "[integration]") {
    const ScratchDirectory scratch;
    const auto file = join_path(join_path(scratch.path(), "no_such_dir"), "out.txt");

    const auto result = write_file_atomic(file, "data");

    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == IoErrorCode::NotFound);
    CHECK_FALSE(path_exists(file));
}

TEST_CASE("Creating a directory over an existing file reports AlreadyExists", "[integration]") {
    const ScratchDirectory scratch;
    const auto file = join_path(scratch.path(), "occupied");
    REQUIRE(write_file_atomic(file, "x").ok());

    const auto result = create_directories(file);

    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == IoErrorCode::AlreadyExists);
}

TEST_CASE("Text reads reject contents that are not UTF-8 while byte reads keep them", "[integration]") {
    const ScratchDirectory scratch;
    const auto file = join_path(scratch.path(), "broken.txt");
    REQUIRE(write_file_atomic(file, "bad\xC3\x28 bytes").ok());

    const auto text = read_text_file(file);
    const auto bytes = read_file(file);

    REQUIRE_FALSE(text.ok());
    CHECK(text.error().code == IoErrorCode::InvalidEncoding);
    REQUIRE(bytes.ok());
    CHECK(bytes.value() == "bad\xC3\x28 bytes");
}

TEST_CASE("Long paths beyond MAX_PATH round-trip through every file operation", "[integration]") {
    const ScratchDirectory scratch;
    auto directory = scratch.path();
    for (int level = 0; level < 10; ++level) {
        directory = join_path(directory, std::string(40, static_cast<char>('a' + level)));
    }
    REQUIRE(directory.str().size() > 400);
    REQUIRE(create_directories(directory).ok());
    const auto file = join_path(directory, "payload.txt");

    REQUIRE(write_file_atomic(file, "deep").ok());

    REQUIRE(path_exists(file));
    CHECK(read_text_file(file).value() == "deep");
}

TEST_CASE("A checked-in Cyrillic fixture reads through a non-normalized path", "[integration]") {
    const auto fixture = require_path(std::string(ENGINE_TEST_FIXTURE_DIR) + "/core/text/./привет_мир.txt");

    const auto contents = read_text_file(fixture);

    REQUIRE(contents.ok());
    CHECK(contents.value() == "Привет, мир!\n");
    CHECK(fixture.str().find("/./") == std::string::npos);
}

TEST_CASE("remove_all removes a tree and treats a missing path as success", "[integration]") {
    const ScratchDirectory scratch;
    const auto nested = join_path(join_path(scratch.path(), "tree"), "leaf");
    REQUIRE(create_directories(nested).ok());
    REQUIRE(write_file_atomic(join_path(nested, "f.txt"), "1").ok());

    REQUIRE(remove_all(join_path(scratch.path(), "tree")).ok());

    CHECK_FALSE(path_exists(join_path(scratch.path(), "tree")));
    CHECK(remove_all(join_path(scratch.path(), "tree")).ok());
}

TEST_CASE("IoError descriptions name the code and the path", "[unit]") {
    const engine::core::IoError error{IoErrorCode::AccessDenied, "/locked/file"};

    CHECK(describe(error) == "AccessDenied: /locked/file");
}
