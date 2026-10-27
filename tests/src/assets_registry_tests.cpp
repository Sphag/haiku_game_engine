// Asset registry (US-018), in memory: SHA-256 digests, record validation, GUID and path conflicts, dependency edges, removal,
// moves, and closures. CPU-only; these run on every OS.

#include "engine/assets/asset_registry.hpp"

#include "engine/assets/content_hash.hpp"

#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using engine::assets::AssetRecord;
using engine::assets::AssetRegistry;
using engine::assets::ContentHash;
using engine::assets::MissingDependency;
using engine::assets::RegistryErrorCode;
using engine::core::Guid;
using engine::core::Path;

namespace {

AssetRecord make_record(const Guid& guid, std::string_view source, std::vector<Guid> dependencies = {}) {
    auto path = Path::from_utf8(source);
    REQUIRE(path.ok());
    return AssetRecord{guid, std::move(path).value(), "texture", "{}", ContentHash{}, std::move(dependencies)};
}

Path path_of(std::string_view text) {
    auto path = Path::from_utf8(text);
    REQUIRE(path.ok());
    return std::move(path).value();
}

}  // namespace

TEST_CASE("SHA-256 matches the FIPS 180-4 vectors at every padding boundary", "[unit]") {
    // The 55 and 56 byte inputs are the last sizes that fit the length in one block and the first that need two.
    CHECK(ContentHash::of("").to_hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(ContentHash::of("abc").to_hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(ContentHash::of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").to_hex() ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(ContentHash::of(std::string(55, 'a')).to_hex() == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(ContentHash::of(std::string(56, 'a')).to_hex() == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(ContentHash::of(std::string(1000000, 'a')).to_hex() ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("Content hashes differ for one changed byte", "[unit]") {
    CHECK(ContentHash::of("brick texels\n") != ContentHash::of("brick texelS\n"));
    CHECK(ContentHash::of("brick texels\n") == ContentHash::of("brick texels\n"));
}

TEST_CASE("The registry rejects invalid records and changes nothing", "[unit]") {
    AssetRegistry registry;
    const Guid kept = Guid::generate();
    REQUIRE(registry.add(make_record(kept, "textures/kept.tex")).ok());

    const auto nil = registry.add(make_record(Guid{}, "textures/nil.tex"));
    CHECK_FALSE(nil.ok());
    CHECK(nil.error().code == RegistryErrorCode::NilGuid);

    const auto absolute = registry.add(make_record(Guid::generate(), "/abs/x.tex"));
    CHECK(absolute.error().code == RegistryErrorCode::InvalidPath);

    const auto empty_path = registry.add(make_record(Guid::generate(), "."));
    CHECK(empty_path.error().code == RegistryErrorCode::InvalidPath);

    AssetRecord no_importer = make_record(Guid::generate(), "textures/none.tex");
    no_importer.importer.clear();
    CHECK(registry.add(std::move(no_importer)).error().code == RegistryErrorCode::EmptyImporter);

    const Guid self = Guid::generate();
    const auto self_dependent = registry.add(make_record(self, "textures/self.tex", {self}));
    CHECK(self_dependent.error().code == RegistryErrorCode::SelfDependency);

    CHECK(registry.size() == 1);
    CHECK(registry.find(kept) != nullptr);
}

TEST_CASE("A duplicate GUID or path is a conflict, and the first record is kept", "[unit]") {
    AssetRegistry registry;
    const Guid guid = Guid::generate();
    REQUIRE(registry.add(make_record(guid, "textures/brick.tex")).ok());

    const auto same_guid = registry.add(make_record(guid, "textures/other.tex"));
    CHECK(same_guid.error().code == RegistryErrorCode::GuidConflict);

    const auto same_path = registry.add(make_record(Guid::generate(), "textures/brick.tex"));
    CHECK(same_path.error().code == RegistryErrorCode::PathConflict);

    REQUIRE(registry.size() == 1);
    const auto* kept = registry.find(guid);
    REQUIRE(kept != nullptr);
    CHECK(kept->source_path.str() == "textures/brick.tex");
    CHECK(registry.find_by_path(path_of("textures/brick.tex")) == kept);
    CHECK(registry.find_by_path(path_of("textures/other.tex")) == nullptr);
}

TEST_CASE("Dependencies are stored sorted and unique, and unregistered targets are reported as missing", "[unit]") {
    AssetRegistry registry;
    const Guid brick = Guid::generate();
    const Guid wall = Guid::generate();
    const Guid absent = Guid::generate();
    REQUIRE(registry.add(make_record(brick, "textures/brick.tex")).ok());
    REQUIRE(registry.add(make_record(wall, "materials/wall.mat", {absent, brick, absent})).ok());

    const auto* record = registry.find(wall);
    REQUIRE(record != nullptr);
    std::vector<Guid> expected{brick, absent};
    std::sort(expected.begin(), expected.end());
    CHECK(record->dependencies == expected);

    const std::vector<MissingDependency> missing = registry.missing_dependencies();
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].asset == wall);
    CHECK(missing[0].dependency == absent);
    CHECK(registry.dependents(brick) == std::vector<Guid>{wall});
}

TEST_CASE("Removing an asset reports its dependents, whose edges then name a missing dependency", "[unit]") {
    AssetRegistry registry;
    const Guid brick = Guid::generate();
    const Guid wall = Guid::generate();
    REQUIRE(registry.add(make_record(brick, "textures/brick.tex")).ok());
    REQUIRE(registry.add(make_record(wall, "materials/wall.mat", {brick})).ok());
    CHECK(registry.missing_dependencies().empty());

    const auto removed = registry.remove(brick);
    REQUIRE(removed.ok());
    CHECK(removed.value() == std::vector<Guid>{wall});
    CHECK(registry.find(brick) == nullptr);
    CHECK(registry.find_by_path(path_of("textures/brick.tex")) == nullptr);

    const std::vector<MissingDependency> missing = registry.missing_dependencies();
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].asset == wall);
    CHECK(missing[0].dependency == brick);

    // The path is free again, and a second removal of the same GUID is NotFound.
    CHECK(registry.add(make_record(Guid::generate(), "textures/brick.tex")).ok());
    CHECK(registry.remove(brick).error().code == RegistryErrorCode::NotFound);
}

TEST_CASE("Moving an asset changes its path only and keeps its GUID and edges", "[unit]") {
    AssetRegistry registry;
    const Guid brick = Guid::generate();
    const Guid wall = Guid::generate();
    const Guid other = Guid::generate();
    REQUIRE(registry.add(make_record(brick, "textures/brick.tex")).ok());
    REQUIRE(registry.add(make_record(wall, "materials/wall.mat", {brick})).ok());
    REQUIRE(registry.add(make_record(other, "textures/other.tex")).ok());

    REQUIRE(registry.move(brick, path_of("texture_library/brick.tex")).ok());
    REQUIRE(registry.find(brick) != nullptr);
    CHECK(registry.find(brick)->source_path.str() == "texture_library/brick.tex");
    CHECK(registry.find_by_path(path_of("textures/brick.tex")) == nullptr);
    CHECK(registry.find_by_path(path_of("texture_library/brick.tex")) == registry.find(brick));
    CHECK(registry.dependents(brick) == std::vector<Guid>{wall});
    CHECK(registry.missing_dependencies().empty());

    // A move onto another asset's path is refused, and the record stays where it was.
    const auto taken = registry.move(brick, path_of("textures/other.tex"));
    CHECK(taken.error().code == RegistryErrorCode::PathConflict);
    CHECK(registry.find(brick)->source_path.str() == "texture_library/brick.tex");
    CHECK(registry.move(Guid::generate(), path_of("x/y.tex")).error().code == RegistryErrorCode::NotFound);
    CHECK(registry.move(brick, path_of("/absolute.tex")).error().code == RegistryErrorCode::InvalidPath);
}

TEST_CASE("The dependency closure is transitive, leaves out the asset, and ends on a cycle", "[unit]") {
    AssetRegistry registry;
    const Guid a = Guid::generate();
    const Guid b = Guid::generate();
    const Guid c = Guid::generate();
    const Guid absent = Guid::generate();
    REQUIRE(registry.add(make_record(c, "c.tex")).ok());
    REQUIRE(registry.add(make_record(b, "b.mat", {c, absent})).ok());
    REQUIRE(registry.add(make_record(a, "a.scene", {b})).ok());

    std::vector<Guid> expected{b, c, absent};
    std::sort(expected.begin(), expected.end());
    CHECK(registry.dependency_closure(a) == expected);
    CHECK(registry.dependency_closure(c).empty());
    CHECK(registry.dependency_closure(Guid::generate()).empty());

    // A cycle is allowed by the registry, but the closure must still finish and must not contain the start.
    AssetRegistry cyclic;
    REQUIRE(cyclic.add(make_record(a, "a.tex", {b})).ok());
    REQUIRE(cyclic.add(make_record(b, "b.tex", {a})).ok());
    CHECK(cyclic.dependency_closure(a) == std::vector<Guid>{b});
}

TEST_CASE("Records are listed in GUID order", "[unit]") {
    AssetRegistry registry;
    std::vector<Guid> guids;
    for (int i = 0; i < 20; ++i) {
        guids.push_back(Guid::generate());
        REQUIRE(registry.add(make_record(guids.back(), "asset_" + std::to_string(i) + ".tex")).ok());
    }
    const auto listed = registry.records();
    REQUIRE(listed.size() == guids.size());
    for (std::size_t i = 1; i < listed.size(); ++i) {
        CHECK(listed[i - 1]->guid < listed[i]->guid);
    }
}
