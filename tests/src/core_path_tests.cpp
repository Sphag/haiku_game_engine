#include "engine/core/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using engine::core::extended_length_path_text;
using engine::core::Path;
using engine::core::PathError;

namespace {

std::string normalized(std::string_view text) {
    const auto path = Path::from_utf8(text);
    REQUIRE(path.ok());
    return path.value().str();
}

}  // namespace

TEST_CASE("Paths normalize separators, dots, and trailing slashes", "[unit]") {
    CHECK(normalized("a\\b//c/./d/../e/") == "a/b/c/e");
    CHECK(normalized("./assets/./textures/") == "assets/textures");
    CHECK(normalized("") == ".");
    CHECK(normalized(".") == ".");
}

TEST_CASE("Path roots are preserved", "[unit]") {
    CHECK(normalized("/") == "/");
    CHECK(normalized("/usr//local/../bin") == "/usr/bin");
    CHECK(normalized("C:\\dev\\..\\engine") == "C:/engine");
    CHECK(normalized("//server/share/dir/../file") == "//server/share/file");
}

TEST_CASE("Relative paths keep leading parent components", "[unit]") {
    CHECK(normalized("../up") == "../up");
    CHECK(normalized("a/../..") == "..");
}

TEST_CASE("Absolute paths cannot climb above their root", "[unit]") {
    const auto result = Path::from_utf8("/a/../..");

    REQUIRE_FALSE(result.ok());
    CHECK(result.error() == PathError::EscapesRoot);
}

TEST_CASE("Paths reject embedded NUL and invalid UTF-8", "[unit]") {
    const std::string with_nul("a\0b", 3);

    CHECK(Path::from_utf8(with_nul).error() == PathError::EmbeddedNul);
    CHECK(Path::from_utf8("bad\xC3\x28name").error() == PathError::InvalidUtf8);
}

TEST_CASE("Cyrillic path text is preserved byte for byte", "[unit]") {
    CHECK(normalized("тест\\./папка//файл.txt") == "тест/папка/файл.txt");
}

TEST_CASE("Path join normalizes the combined result", "[unit]") {
    const auto base = Path::from_utf8("/data/world");
    REQUIRE(base.ok());

    const auto joined = base.value().join("../cells/Привет.bin");

    REQUIRE(joined.ok());
    CHECK(joined.value().str() == "/data/cells/Привет.bin");
    CHECK(joined.value().file_name() == "Привет.bin");
}

TEST_CASE("Absolute detection covers POSIX, drive, and UNC roots", "[unit]") {
    CHECK(Path::from_utf8("/x").value().is_absolute());
    CHECK(Path::from_utf8("D:\\x").value().is_absolute());
    CHECK(Path::from_utf8("//server/share").value().is_absolute());
    CHECK_FALSE(Path::from_utf8("x/y").value().is_absolute());
}

TEST_CASE("Windows extended-length form is used only for long absolute paths", "[unit]") {
    const std::string short_path = "C:/dev/engine";
    const std::string long_tail(300, 'x');

    CHECK(extended_length_path_text(short_path) == "C:\\dev\\engine");
    CHECK(extended_length_path_text("C:/" + long_tail) == "\\\\?\\C:\\" + long_tail);
    CHECK(extended_length_path_text("//server/share/" + long_tail) == "\\\\?\\UNC\\server\\share\\" + long_tail);
    CHECK(extended_length_path_text("relative/" + long_tail) == "relative\\" + long_tail);
}
