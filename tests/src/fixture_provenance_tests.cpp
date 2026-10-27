// Fixture provenance (US-075): tests/fixtures/PROVENANCE.md lists every fixture with its SHA-256, source, and license. This test
// reads that table, hashes each file on disk, and fails on a missing, extra, or changed file. The check uses only the repository
// files, so it runs with no download and no private asset.

#include "engine/assets/content_hash.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct ProvenanceRow {
    std::string sha256;
    std::string source;
    std::string license;
};

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// Splits one table row into its trimmed cells. A row is "| a | b | c | d |".
std::vector<std::string> row_cells(const std::string& line) {
    std::vector<std::string> cells;
    std::string::size_type start = line.find('|');
    while (start != std::string::npos) {
        const auto end = line.find('|', start + 1);
        if (end == std::string::npos) {
            break;
        }
        cells.push_back(trim(line.substr(start + 1, end - start - 1)));
        start = end;
    }
    return cells;
}

std::string strip_backticks(const std::string& text) {
    if (text.size() >= 2 && text.front() == '`' && text.back() == '`') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

// Forward-slash UTF-8 path of `file` relative to `root`, so it matches the table on every host.
std::string relative_utf8(const std::filesystem::path& file, const std::filesystem::path& root) {
    const auto relative = file.lexically_relative(root).u8string();
    std::string text(relative.begin(), relative.end());
    for (auto& ch : text) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    return text;
}

}  // namespace

TEST_CASE("Fixtures: every file has provenance and an unchanged hash", "[unit]") {
    const std::string root_text = ENGINE_TEST_FIXTURE_DIR;
    const auto provenance_path = engine::core::Path::from_utf8(root_text + "/PROVENANCE.md");
    REQUIRE(provenance_path.ok());
    const auto provenance_text = engine::core::read_text_file(provenance_path.value());
    REQUIRE(provenance_text.ok());

    std::map<std::string, ProvenanceRow> rows;
    std::istringstream lines(provenance_text.value());
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("| `", 0) != 0) {
            continue;
        }
        const auto cells = row_cells(line);
        REQUIRE(cells.size() == 4);
        const std::string file = strip_backticks(cells[0]);
        INFO("row " << file);
        CHECK(rows.count(file) == 0);
        rows[file] = ProvenanceRow{cells[1], cells[2], cells[3]};
    }
    REQUIRE(rows.size() > 0);

    const std::filesystem::path root = std::filesystem::path(root_text);
    std::size_t files_on_disk = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string rel = relative_utf8(entry.path(), root);
        if (rel == "PROVENANCE.md") {
            continue;
        }
        ++files_on_disk;
        INFO("file " << rel);
        const auto row = rows.find(rel);
        REQUIRE(row != rows.end());
        const auto path = engine::core::Path::from_utf8(root_text + "/" + rel);
        REQUIRE(path.ok());
        const auto bytes = engine::core::read_file(path.value());
        REQUIRE(bytes.ok());
        CHECK(engine::assets::ContentHash::of(bytes.value()).to_hex() == row->second.sha256);
    }

    CHECK(files_on_disk == rows.size());
    for (const auto& [file, row] : rows) {
        INFO("row " << file);
        CHECK(!row.source.empty());
        CHECK(!row.license.empty());
    }
}
