// Asset registry (US-018) on a project tree: the fixture project under tests/fixtures/assets/registry_project is copied to a
// scratch directory for each case, so deletion, renaming, and bad sidecars never touch the checked-in files. These tests
// touch the file system, so they are integration tests.

#include "engine/assets/asset_project.hpp"

#include "engine/assets/asset_registry.hpp"
#include "engine/assets/content_hash.hpp"

#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using engine::assets::AssetRecord;
using engine::assets::ContentHash;
using engine::assets::create_asset_metadata;
using engine::assets::MissingDependency;
using engine::assets::ProjectScan;
using engine::assets::RegistryErrorCode;
using engine::assets::scan_project;
using engine::assets::ScanError;
using engine::assets::ScanErrorCode;
using engine::core::Guid;
using engine::core::Path;
namespace fs = std::filesystem;

namespace {

constexpr const char* kBrickGuid = "6b1f2a4e-0c3d-4e5f-8a9b-1c2d3e4f5a60";
constexpr const char* kWallGuid = "7c2e3b5f-1d4e-4f60-9b0c-2d3e4f5a6b71";
constexpr const char* kHallGuid = "8d3f4c60-2e5f-4071-8c1d-3e4f5a6b7c82";

// A scratch copy of the fixture project, removed when the case ends.
class ProjectCopy {
public:
    ProjectCopy() {
        const fs::path source = fs::path(std::string(ENGINE_TEST_FIXTURE_DIR)) / "assets" / "registry_project";
        root_ = fs::temp_directory_path() / ("engine_asset_registry_" + Guid::generate().to_string());
        fs::create_directories(root_);
        fs::copy(source, root_, fs::copy_options::recursive);
    }
    ~ProjectCopy() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    ProjectCopy(const ProjectCopy&) = delete;
    ProjectCopy& operator=(const ProjectCopy&) = delete;

    [[nodiscard]] const fs::path& native() const noexcept {
        return root_;
    }

    [[nodiscard]] Path path() const {
        const auto text = root_.generic_u8string();
        auto parsed = Path::from_utf8(std::string(reinterpret_cast<const char*>(text.data()), text.size()));
        REQUIRE(parsed.ok());
        return std::move(parsed).value();
    }

    // Writes or replaces a file under the copy, creating its directories.
    void write(const std::string& relative, const std::string& text) const {
        fs::create_directories((root_ / fs::path(relative)).parent_path());
        std::ofstream out(root_ / fs::path(relative), std::ios::binary);
        out << text;
    }

private:
    fs::path root_;
};

ProjectScan scan_or_fail(const ProjectCopy& project) {
    auto scanned = scan_project(project.path());
    if (!scanned.ok()) {
        FAIL("scan failed: " << scanned.error().message);
    }
    return std::move(scanned).value();
}

Guid guid(const char* text) {
    auto parsed = Guid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

}  // namespace

TEST_CASE("A fixture project registers each sidecar with its GUID, path, settings, hash, and dependencies", "[integration]") {
    const ProjectCopy project;
    const ProjectScan scan = scan_or_fail(project);

    REQUIRE(scan.registry.size() == 3);
    CHECK(scan.unregistered == std::vector<std::string>{"notes/readme.txt"});
    CHECK(scan.orphan_metadata.empty());
    CHECK(scan.registry.missing_dependencies().empty());

    const AssetRecord* brick = scan.registry.find(guid(kBrickGuid));
    REQUIRE(brick != nullptr);
    CHECK(brick->source_path.str() == "textures/brick.tex");
    CHECK(brick->importer == "texture");
    CHECK(brick->importer_settings == R"({"srgb":true})");
    CHECK(brick->content_hash == ContentHash::of("brick texels\n"));
    CHECK(brick->dependencies.empty());

    // The source name is Cyrillic. The registry keys it by the same UTF-8 path the sidecar was written under.
    const AssetRecord* wall = scan.registry.find_by_path(Path::from_utf8("materials/стена.mat").value());
    REQUIRE(wall != nullptr);
    CHECK(wall->guid == guid(kWallGuid));
    CHECK(wall->dependencies == std::vector<Guid>{guid(kBrickGuid)});

    const std::vector<Guid> closure = scan.registry.dependency_closure(guid(kHallGuid));
    CHECK(closure.size() == 2);
    CHECK(std::find(closure.begin(), closure.end(), guid(kWallGuid)) != closure.end());
    CHECK(std::find(closure.begin(), closure.end(), guid(kBrickGuid)) != closure.end());
}

TEST_CASE("Renaming and moving a folder of sources with their sidecars keeps every GUID and reference", "[integration]") {
    const ProjectCopy project;
    fs::rename(project.native() / "textures", project.native() / "texture_library");

    const ProjectScan scan = scan_or_fail(project);
    REQUIRE(scan.registry.size() == 3);
    const AssetRecord* brick = scan.registry.find(guid(kBrickGuid));
    REQUIRE(brick != nullptr);
    CHECK(brick->source_path.str() == "texture_library/brick.tex");
    CHECK(scan.registry.find_by_path(Path::from_utf8("textures/brick.tex").value()) == nullptr);
    CHECK(scan.registry.dependents(guid(kBrickGuid)) == std::vector<Guid>{guid(kWallGuid)});
    CHECK(scan.registry.missing_dependencies().empty());
}

TEST_CASE("Renaming a single source and its sidecar keeps its GUID", "[integration]") {
    const ProjectCopy project;
    fs::rename(project.native() / "textures" / "brick.tex", project.native() / "textures" / "brick_renamed.tex");
    fs::rename(project.native() / "textures" / "brick.tex.meta", project.native() / "textures" / "brick_renamed.tex.meta");

    const ProjectScan scan = scan_or_fail(project);
    const AssetRecord* brick = scan.registry.find(guid(kBrickGuid));
    REQUIRE(brick != nullptr);
    CHECK(brick->source_path.str() == "textures/brick_renamed.tex");
    CHECK(scan.registry.missing_dependencies().empty());
}

TEST_CASE("Deleting an asset with its sidecar leaves its dependents with a missing dependency", "[integration]") {
    const ProjectCopy project;
    fs::remove(project.native() / "textures" / "brick.tex");
    fs::remove(project.native() / "textures" / "brick.tex.meta");

    const ProjectScan scan = scan_or_fail(project);
    CHECK(scan.registry.size() == 2);
    CHECK(scan.registry.find(guid(kBrickGuid)) == nullptr);
    CHECK(scan.orphan_metadata.empty());

    const std::vector<MissingDependency> missing = scan.registry.missing_dependencies();
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].asset == guid(kWallGuid));
    CHECK(missing[0].dependency == guid(kBrickGuid));
    CHECK(scan.registry.dependents(guid(kBrickGuid)) == std::vector<Guid>{guid(kWallGuid)});
    CHECK(scan.registry.dependency_closure(guid(kHallGuid)).size() == 2);
}

TEST_CASE("Deleting only a source leaves its sidecar orphaned and the asset unregistered", "[integration]") {
    const ProjectCopy project;
    fs::remove(project.native() / "textures" / "brick.tex");

    const ProjectScan scan = scan_or_fail(project);
    CHECK(scan.registry.find(guid(kBrickGuid)) == nullptr);
    CHECK(scan.orphan_metadata == std::vector<std::string>{"textures/brick.tex.meta"});
    CHECK(scan.unregistered == std::vector<std::string>{"notes/readme.txt"});
    CHECK(scan.registry.missing_dependencies().size() == 1);
}

TEST_CASE("Moving a source without its sidecar gives it a new GUID, and its dependents stay broken", "[integration]") {
    const ProjectCopy project;
    fs::rename(project.native() / "textures" / "brick.tex", project.native() / "textures" / "brick_moved.tex");

    const ProjectScan scan = scan_or_fail(project);
    CHECK(scan.registry.find(guid(kBrickGuid)) == nullptr);
    CHECK(scan.orphan_metadata == std::vector<std::string>{"textures/brick.tex.meta"});
    const std::vector<std::string> expected_unregistered{"notes/readme.txt", "textures/brick_moved.tex"};
    CHECK(scan.unregistered == expected_unregistered);
    const std::vector<MissingDependency> missing = scan.registry.missing_dependencies();
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].dependency == guid(kBrickGuid));
}

TEST_CASE("A copy of a source with its sidecar is rejected as a GUID conflict", "[integration]") {
    const ProjectCopy project;
    fs::copy_file(project.native() / "textures" / "brick.tex", project.native() / "textures" / "brick_copy.tex");
    fs::copy_file(project.native() / "textures" / "brick.tex.meta", project.native() / "textures" / "brick_copy.tex.meta");

    const auto scanned = scan_project(project.path());
    REQUIRE_FALSE(scanned.ok());
    CHECK(scanned.error().code == ScanErrorCode::GuidConflict);
    CHECK(scanned.error().message.find(kBrickGuid) != std::string::npos);
}

TEST_CASE("A malformed sidecar fails the whole scan with a specific code", "[integration]") {
    const std::string valid_brick = R"({
  "format": "engine.asset_meta",
  "version": 1,
  "guid": "6b1f2a4e-0c3d-4e5f-8a9b-1c2d3e4f5a60",
  "importer": { "type": "texture", "settings": {} }
})";

    const ProjectCopy project;
    project.write("textures/brick.tex.meta", valid_brick);
    REQUIRE(scan_project(project.path()).ok());

    SECTION("an unknown key") {
        project.write("textures/brick.tex.meta", R"({"format":"engine.asset_meta","version":1,"guid":"6b1f2a4e-0c3d-4e5f-8a9b-1c2d3e4f5a60","importer":{"type":"texture"},"extra":1})");
        CHECK(scan_project(project.path()).error().code == ScanErrorCode::Malformed);
    }
    SECTION("a different format name") {
        project.write("textures/brick.tex.meta", R"({"format":"engine.scene","version":1,"guid":"6b1f2a4e-0c3d-4e5f-8a9b-1c2d3e4f5a60","importer":{"type":"texture"}})");
        CHECK(scan_project(project.path()).error().code == ScanErrorCode::WrongFormat);
    }
    SECTION("an unsupported version") {
        project.write("textures/brick.tex.meta", R"({"format":"engine.asset_meta","version":2,"guid":"6b1f2a4e-0c3d-4e5f-8a9b-1c2d3e4f5a60","importer":{"type":"texture"}})");
        CHECK(scan_project(project.path()).error().code == ScanErrorCode::UnsupportedVersion);
    }
    SECTION("a nil GUID") {
        project.write("textures/brick.tex.meta", R"({"format":"engine.asset_meta","version":1,"guid":"00000000-0000-0000-0000-000000000000","importer":{"type":"texture"}})");
        CHECK(scan_project(project.path()).error().code == ScanErrorCode::Malformed);
    }
    SECTION("a dependency that is not a GUID") {
        project.write("textures/brick.tex.meta", R"({"format":"engine.asset_meta","version":1,"guid":"6b1f2a4e-0c3d-4e5f-8a9b-1c2d3e4f5a60","importer":{"type":"texture"},"dependencies":["nope"]})");
        CHECK(scan_project(project.path()).error().code == ScanErrorCode::Malformed);
    }
    SECTION("a sidecar that is not JSON") {
        project.write("textures/brick.tex.meta", "{ not json");
        CHECK(scan_project(project.path()).error().code == ScanErrorCode::Malformed);
    }
}

TEST_CASE("A sidecar created for an unregistered source is registered by the next scan", "[integration]") {
    const ProjectCopy project;
    const auto created = create_asset_metadata(project.path(), Path::from_utf8("notes/readme.txt").value(), "text",
                                               R"({"encoding":"utf8"})");
    REQUIRE(created.ok());

    const ProjectScan scan = scan_or_fail(project);
    CHECK(scan.unregistered.empty());
    const AssetRecord* readme = scan.registry.find(created.value());
    REQUIRE(readme != nullptr);
    CHECK(readme->source_path.str() == "notes/readme.txt");
    CHECK(readme->importer_settings == R"({"encoding":"utf8"})");
}

TEST_CASE("create_asset_metadata refuses a missing source, an existing sidecar, and settings that are not an object", "[integration]") {
    const ProjectCopy project;
    CHECK(create_asset_metadata(project.path(), Path::from_utf8("notes/missing.txt").value(), "text", "{}").error().code ==
          ScanErrorCode::MissingSource);
    CHECK(create_asset_metadata(project.path(), Path::from_utf8("textures/brick.tex").value(), "texture", "{}").error().code ==
          ScanErrorCode::AlreadyExists);
    CHECK(create_asset_metadata(project.path(), Path::from_utf8("notes/readme.txt").value(), "text", "[1,2]").error().code ==
          ScanErrorCode::Malformed);
    CHECK(create_asset_metadata(project.path(), Path::from_utf8("notes/readme.txt").value(), "", "{}").error().code ==
          ScanErrorCode::Malformed);
}

TEST_CASE("A scan of a path that is not a project directory fails with Io", "[integration]") {
    const ProjectCopy project;
    const auto scanned = scan_project(Path::from_utf8(project.path().str() + "/no_such_folder").value());
    REQUIRE_FALSE(scanned.ok());
    CHECK(scanned.error().code == ScanErrorCode::Io);
}
