// Resource packs (US-060): the closure a build packs, the byte layout, and every way a damaged or incomplete file is refused.
// These checks run on in-memory bytes, so they need no files. Packs are also built and opened from disk by the CTest round trip.

#include "engine/assets/asset_registry.hpp"
#include "engine/assets/content_hash.hpp"
#include "engine/assets/cooked_container.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/pack/pack.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using engine::assets::AssetRecord;
using engine::assets::AssetRegistry;
using engine::assets::ContentHash;
using engine::core::Guid;
using engine::core::Path;
using engine::core::Result;
using engine::pack::CookedLookup;
using engine::pack::PackError;
using engine::pack::PackErrorCode;
using engine::pack::PackReader;

namespace {

const Guid kA = Guid::parse("3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b01").value();
const Guid kB = Guid::parse("3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b02").value();
const Guid kC = Guid::parse("3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b03").value();
const Guid kMissing = Guid::parse("3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b04").value();

// Every member is named, in declaration order, because a record has no default constructor.
AssetRecord record(const Guid& guid, const char* source, std::vector<Guid> dependencies) {
    return AssetRecord{guid, Path::from_utf8(source).value(), "blob", "{}", ContentHash{}, std::move(dependencies)};
}

// A lookup over an in-memory map of cooked files. The map must outlive the lookup.
CookedLookup cooked_from(const std::map<Guid, std::string>& files) {
    return [&files](const Guid& guid) -> Result<std::string, std::string> {
        const auto found = files.find(guid);
        if (found == files.end()) {
            return engine::core::failure(std::string("no cooked file for this asset"));
        }
        return engine::core::success(found->second);
    };
}

void put_little_endian(std::string& out, std::uint64_t value, int byte_count) {
    for (int i = 0; i < byte_count; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
    }
}

// Lays out a pack from an index text and a payload, with correct hashes. Tests use it to build files the builder would refuse.
std::string craft_pack(const std::string& index_text, const std::string& payload) {
    std::string out = "OWPK";
    put_little_endian(out, engine::pack::kPackVersion, 4);
    put_little_endian(out, index_text.size(), 8);
    const auto hash = ContentHash::of(index_text).bytes();
    out.append(reinterpret_cast<const char*>(hash.data()), hash.size());
    out.append(index_text);
    out.append(payload);
    return out;
}

// A registry of A -> B, with C unrelated. Each has a cooked payload.
struct Fixture {
    AssetRegistry registry;
    std::map<Guid, std::string> cooked{{kA, "alpha-cooked"}, {kB, "bravo-cooked"}, {kC, "charlie-cooked"}};

    Fixture() {
        REQUIRE(registry.add(record(kA, "scenes/a.json", {kB})).ok());
        REQUIRE(registry.add(record(kB, "data/b.bin", {})).ok());
        REQUIRE(registry.add(record(kC, "data/c.bin", {})).ok());
    }
};

std::string build_ok(const Fixture& fixture, const std::vector<Guid>& roots) {
    auto bytes = engine::pack::build_pack(fixture.registry, roots, cooked_from(fixture.cooked));
    REQUIRE(bytes.ok());
    return bytes.value();
}

}  // namespace

TEST_CASE("Pack: a build holds the closure of its roots and nothing else", "[unit]") {
    const Fixture fixture;
    const auto bytes = build_ok(fixture, {kA});
    auto reader = PackReader::open(bytes);

    REQUIRE(reader.ok());
    CHECK(reader.value().roots() == std::vector<Guid>{kA});
    CHECK(reader.value().entries().size() == 2);
    CHECK(reader.value().find(kA) != nullptr);
    CHECK(reader.value().find(kB) != nullptr);
    CHECK(reader.value().find(kC) == nullptr);
}

TEST_CASE("Pack: each payload round trips byte for byte, with its hash, source, and dependencies", "[unit]") {
    const Fixture fixture;
    auto reader = PackReader::open(build_ok(fixture, {kA}));
    REQUIRE(reader.ok());

    const auto* a = reader.value().find(kA);
    REQUIRE(a != nullptr);
    CHECK(reader.value().payload(*a) == "alpha-cooked");
    CHECK(a->source == "scenes/a.json");
    CHECK(a->hash == ContentHash::of("alpha-cooked").to_hex());
    CHECK(a->dependencies == std::vector<Guid>{kB});
    CHECK(reader.value().payload(*reader.value().find(kB)) == "bravo-cooked");
}

TEST_CASE("Pack: a missing dependency aborts the build and names the GUID", "[unit]") {
    AssetRegistry registry;
    REQUIRE(registry.add(record(kA, "scenes/a.json", {kMissing})).ok());
    const std::map<Guid, std::string> cooked{{kA, "alpha-cooked"}};

    auto bytes = engine::pack::build_pack(registry, {kA}, cooked_from(cooked));

    REQUIRE_FALSE(bytes.ok());
    CHECK(bytes.error().code == PackErrorCode::MissingDependency);
    CHECK(bytes.error().guid == kMissing.to_string());
}

TEST_CASE("Pack: a missing cooked payload, an unknown root, and an empty root list abort the build", "[unit]") {
    Fixture fixture;

    const std::map<Guid, std::string> without_b{{kA, "alpha-cooked"}};
    auto no_cooked = engine::pack::build_pack(fixture.registry, {kA}, cooked_from(without_b));
    REQUIRE_FALSE(no_cooked.ok());
    CHECK(no_cooked.error().code == PackErrorCode::MissingCooked);
    CHECK(no_cooked.error().guid == kB.to_string());

    auto unknown = engine::pack::build_pack(fixture.registry, {kMissing}, cooked_from(fixture.cooked));
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().code == PackErrorCode::UnknownRoot);

    auto empty = engine::pack::build_pack(fixture.registry, {}, cooked_from(fixture.cooked));
    REQUIRE_FALSE(empty.ok());
    CHECK(empty.error().code == PackErrorCode::NoRoots);
}

TEST_CASE("Pack: the bytes do not depend on the order of the roots", "[unit]") {
    const Fixture fixture;
    CHECK(build_ok(fixture, {kA, kC}) == build_ok(fixture, {kC, kA}));
    CHECK(build_ok(fixture, {kA, kA}) == build_ok(fixture, {kA}));
}

TEST_CASE("Pack: a dependency cycle packs each asset once and terminates", "[unit]") {
    AssetRegistry registry;
    REQUIRE(registry.add(record(kA, "a.bin", {kB})).ok());
    REQUIRE(registry.add(record(kB, "b.bin", {kA})).ok());
    const std::map<Guid, std::string> cooked{{kA, "one"}, {kB, "two"}};

    auto bytes = engine::pack::build_pack(registry, {kA}, cooked_from(cooked));
    REQUIRE(bytes.ok());
    auto reader = PackReader::open(bytes.value());
    REQUIRE(reader.ok());
    CHECK(reader.value().entries().size() == 2);
}

TEST_CASE("Pack: a damaged payload, a damaged index, or a truncated file is refused", "[unit]") {
    const Fixture fixture;
    const auto bytes = build_ok(fixture, {kA});

    std::string payload_damaged = bytes;
    payload_damaged.back() = static_cast<char>(payload_damaged.back() ^ 0x01);
    auto payload = PackReader::open(payload_damaged);
    REQUIRE_FALSE(payload.ok());
    CHECK(payload.error().code == PackErrorCode::IntegrityFailed);

    std::string index_damaged = bytes;
    index_damaged[engine::pack::kPackHeaderBytes + 3] = static_cast<char>(index_damaged[engine::pack::kPackHeaderBytes + 3] ^ 0x01);
    auto index = PackReader::open(index_damaged);
    REQUIRE_FALSE(index.ok());
    CHECK(index.error().code == PackErrorCode::IntegrityFailed);

    auto truncated = PackReader::open(bytes.substr(0, bytes.size() - 1));
    REQUIRE_FALSE(truncated.ok());
    CHECK(truncated.error().code == PackErrorCode::Truncated);

    auto header_only = PackReader::open(bytes.substr(0, 10));
    REQUIRE_FALSE(header_only.ok());
    CHECK(header_only.error().code == PackErrorCode::Truncated);
}

TEST_CASE("Pack: a wrong magic, version, or format name is refused", "[unit]") {
    const Fixture fixture;
    const auto bytes = build_ok(fixture, {kA});

    std::string magic = bytes;
    magic[0] = 'X';
    auto wrong_magic = PackReader::open(magic);
    REQUIRE_FALSE(wrong_magic.ok());
    CHECK(wrong_magic.error().code == PackErrorCode::WrongFormat);

    std::string version = bytes;
    version[4] = 2;
    auto wrong_version = PackReader::open(version);
    REQUIRE_FALSE(wrong_version.ok());
    CHECK(wrong_version.error().code == PackErrorCode::UnsupportedVersion);

    const std::string other_format = R"({"format":"engine.scene","version":1,"roots":[],"entries":[]})";
    auto wrong_format = PackReader::open(craft_pack(other_format, ""));
    REQUIRE_FALSE(wrong_format.ok());
    CHECK(wrong_format.error().code == PackErrorCode::WrongFormat);
}

TEST_CASE("Pack: an entry whose dependency is not in the pack is refused as an incomplete closure", "[unit]") {
    const std::string payload = "alpha";
    const std::string index = R"({"entries":[{"dependencies":["3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b04"],"guid":"3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b01","hash":")" +
                              ContentHash::of(payload).to_hex() +
                              R"(","offset":0,"size":5,"source":"a.json"}],"format":"engine.pack","roots":["3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b01"],"version":1})";

    auto reader = PackReader::open(craft_pack(index, payload));

    REQUIRE_FALSE(reader.ok());
    CHECK(reader.error().code == PackErrorCode::IncompleteClosure);
    CHECK(reader.error().guid == kMissing.to_string());
}

TEST_CASE("Pack: payload ranges must tile the file, with no gap and no trailing bytes", "[unit]") {
    const std::string payload = "alpha!";
    const std::string entry_hash = ContentHash::of("alpha").to_hex();
    const std::string index = R"({"entries":[{"dependencies":[],"guid":"3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b01","hash":")" +
                              entry_hash + R"(","offset":0,"size":5,"source":"a.json"}],"format":"engine.pack","roots":["3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b01"],"version":1})";

    auto trailing = PackReader::open(craft_pack(index, payload));
    REQUIRE_FALSE(trailing.ok());
    CHECK(trailing.error().code == PackErrorCode::Truncated);
}

TEST_CASE("Pack: an index that is not JSON or has a missing member is malformed", "[unit]") {
    auto not_json = PackReader::open(craft_pack("{not json", ""));
    REQUIRE_FALSE(not_json.ok());
    CHECK(not_json.error().code == PackErrorCode::Malformed);

    auto no_entries = PackReader::open(craft_pack(R"({"format":"engine.pack","version":1,"roots":[]})", ""));
    REQUIRE_FALSE(no_entries.ok());
    CHECK(no_entries.error().code == PackErrorCode::Malformed);
}
