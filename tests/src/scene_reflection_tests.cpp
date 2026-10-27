// Component reflection (US-015): descriptors, validation, the catalog, and value checks. CPU-only; these run on every OS.

#include "engine/scene/reflection.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <vector>

using engine::core::Guid;
using engine::scene::ComponentBuilder;
using engine::scene::ComponentCatalog;
using engine::scene::ComponentDescriptor;
using engine::scene::FieldDescriptor;
using engine::scene::FieldKind;
using engine::scene::FieldTraits;
using engine::scene::FieldValue;
using engine::scene::Range;
using engine::scene::ReflectionError;
using engine::scene::ReflectionErrorKind;

namespace {

struct Health {
    std::int32_t hp = 100;
    std::uint32_t regen_ticks = 0;
    float armor = 0.5f;
    bool invulnerable = false;
};

struct Placement {
    std::array<float, 3> position{};
    std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
    Guid prefab;
};

// Shares the type ID "game.health" with Health but is a different C++ type.
struct Stamina {
    std::int32_t value = 0;
};

// Reflected in game code, not in any editor code. The editor functions below never name this type.
struct WaterVolume {
    float depth = 2.0f;
    bool flowing = false;
    std::array<float, 2> flow_direction{1.0f, 0.0f};
};

static_assert(FieldTraits<float>::kind == FieldKind::Float32);
static_assert(FieldTraits<std::array<float, 3>>::kind == FieldKind::Float32x3);
static_assert(FieldTraits<Guid>::kind == FieldKind::Guid);

bool has_kind(const std::vector<ReflectionError>& errors, ReflectionErrorKind kind) {
    for (const ReflectionError& error : errors) {
        if (error.kind == kind) {
            return true;
        }
    }
    return false;
}

template <typename C>
ComponentDescriptor must_build(const ComponentBuilder<C>& builder) {
    auto result = builder.build();
    REQUIRE(result.ok());
    return std::move(result).value();
}

ComponentBuilder<Health> health_builder() {
    ComponentBuilder<Health> builder("game.health", 3);
    builder.field<&Health::hp>("hp", 100, Range{0.0, 999.0});
    builder.field<&Health::regen_ticks>("regen_ticks", 0);
    builder.field<&Health::armor>("armor", 0.5f, Range{0.0, 1.0});
    builder.field<&Health::invulnerable>("invulnerable", false);
    return builder;
}

ComponentBuilder<WaterVolume> water_builder() {
    ComponentBuilder<WaterVolume> builder("game.water_volume", 1);
    builder.field<&WaterVolume::depth>("depth", 2.0f, Range{0.1, 50.0});
    builder.field<&WaterVolume::flowing>("flowing", false);
    builder.field<&WaterVolume::flow_direction>("flow_direction", {1.0f, 0.0f});
    return builder;
}

// Editor code. Written once against the catalog; it never names a component type, so new components need no change here.
std::vector<std::string> describe_catalog(const ComponentCatalog& catalog) {
    std::vector<std::string> lines;
    for (const ComponentDescriptor& component : catalog.components()) {
        lines.push_back(component.type_id + " v" + std::to_string(component.schema_version));
        for (const FieldDescriptor& field : component.fields) {
            lines.push_back("  " + field.name);
        }
    }
    return lines;
}

}  // namespace

TEST_CASE("Reflection hashes type IDs with pinned FNV-1a 64-bit vectors", "[unit]") {
    CHECK(engine::scene::type_id_hash("") == 0xcbf29ce484222325ULL);
    CHECK(engine::scene::type_id_hash("a") == 0xaf63dc4c8601ec8cULL);
    CHECK(engine::scene::type_id_hash("engine.transform") == engine::scene::type_id_hash("engine.transform"));
    CHECK(engine::scene::type_id_hash("engine.transform") != engine::scene::type_id_hash("engine.transforms"));
}

TEST_CASE("Reflection accepts dotted lowercase type IDs and rejects other spellings", "[unit]") {
    const std::array<std::string_view, 4> valid{"a", "engine.transform", "game.door_lock", "game.v2.part1"};
    for (const std::string_view name : valid) {
        CAPTURE(name);
        CHECK(engine::scene::is_valid_type_id(name));
    }

    const std::string too_long(engine::scene::kMaxTypeIdLength + 1, 'a');
    const std::array<std::string_view, 9> invalid{"", "Engine.transform", "engine..transform", ".engine", "engine.",
                                                  "1engine",                "engine-transform",   "engine transform",
                                                  too_long};
    for (const std::string_view name : invalid) {
        CAPTURE(name);
        CHECK_FALSE(engine::scene::is_valid_type_id(name));
    }
}

TEST_CASE("Reflection accepts snake_case field names without dots", "[unit]") {
    CHECK(engine::scene::is_valid_field_name("hp"));
    CHECK(engine::scene::is_valid_field_name("regen_ticks"));
    CHECK_FALSE(engine::scene::is_valid_field_name(""));
    CHECK_FALSE(engine::scene::is_valid_field_name("Hp"));
    CHECK_FALSE(engine::scene::is_valid_field_name("a.b"));
    CHECK_FALSE(engine::scene::is_valid_field_name("_x"));
    CHECK_FALSE(engine::scene::is_valid_field_name("1x"));
    CHECK_FALSE(engine::scene::is_valid_field_name(std::string(engine::scene::kMaxFieldNameLength + 1, 'x')));
}

TEST_CASE("A built component keeps its ID, schema version, and fields in declaration order", "[unit]") {
    const ComponentDescriptor health = must_build(health_builder());

    CHECK(health.type_id == "game.health");
    CHECK(health.schema_version == 3);
    REQUIRE(health.fields.size() == 4);
    CHECK(health.fields[0].name == "hp");
    CHECK(health.fields[2].name == "armor");
    CHECK(health.fields[0].kind() == FieldKind::Int32);
    CHECK(health.fields[2].kind() == FieldKind::Float32);
    CHECK(health.describes<Health>());
    CHECK_FALSE(health.describes<Stamina>());

    Health instance;
    instance.hp = 7;
    instance.armor = 0.9f;
    health.apply_defaults(&instance);
    CHECK(instance.hp == 100);
    CHECK(instance.armor == 0.5f);
}

TEST_CASE("The catalog finds a component by its type ID and by its C++ type", "[unit]") {
    ComponentCatalog catalog;
    REQUIRE(catalog.add(must_build(health_builder())).ok());
    REQUIRE(catalog.add(must_build(water_builder())).ok());

    CHECK(catalog.size() == 2);
    const ComponentDescriptor* by_name = catalog.find("game.water_volume");
    REQUIRE(by_name != nullptr);
    CHECK(by_name->describes<WaterVolume>());
    CHECK(catalog.find<Health>() == catalog.find("game.health"));
    CHECK(catalog.find<Stamina>() == nullptr);
    CHECK(catalog.find("game.missing") == nullptr);
}

TEST_CASE("set_field writes an in-range value and get_field reads it back", "[unit]") {
    ComponentCatalog catalog;
    REQUIRE(catalog.add(must_build(health_builder())).ok());
    const ComponentDescriptor* health = catalog.find<Health>();
    REQUIRE(health != nullptr);

    Health instance;
    const FieldValue new_hp = std::int32_t{250};
    REQUIRE(health->set_field(&instance, "hp", new_hp).ok());
    CHECK(instance.hp == 250);

    const auto read = health->get_field(&instance, "hp");
    REQUIRE(read.ok());
    CHECK(read.value() == new_hp);
}

TEST_CASE("set_field refuses bad input and leaves the component unchanged", "[unit]") {
    ComponentCatalog catalog;
    REQUIRE(catalog.add(must_build(health_builder())).ok());
    const ComponentDescriptor* health = catalog.find<Health>();
    REQUIRE(health != nullptr);

    Health instance;
    instance.hp = 42;

    const FieldValue above_max = std::int32_t{1000};
    const auto too_big = health->set_field(&instance, "hp", above_max);
    REQUIRE_FALSE(too_big.ok());
    CHECK(too_big.error().kind == ReflectionErrorKind::ValueOutOfRange);

    const FieldValue wrong_kind = 3.0f;
    const auto mismatch = health->set_field(&instance, "hp", wrong_kind);
    REQUIRE_FALSE(mismatch.ok());
    CHECK(mismatch.error().kind == ReflectionErrorKind::ValueTypeMismatch);

    const FieldValue not_a_number = std::numeric_limits<float>::quiet_NaN();
    const auto not_finite = health->set_field(&instance, "armor", not_a_number);
    REQUIRE_FALSE(not_finite.ok());
    CHECK(not_finite.error().kind == ReflectionErrorKind::NonFiniteValue);

    const FieldValue armor_above = 1.5f;
    const auto armor_too_big = health->set_field(&instance, "armor", armor_above);
    REQUIRE_FALSE(armor_too_big.ok());
    CHECK(armor_too_big.error().kind == ReflectionErrorKind::ValueOutOfRange);

    const FieldValue any = true;
    const auto unknown = health->set_field(&instance, "shield", any);
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().kind == ReflectionErrorKind::UnknownField);

    CHECK(instance.hp == 42);
    CHECK(instance.armor == 0.5f);
    CHECK_FALSE(instance.invulnerable);
}

TEST_CASE("A float bound accepts the float nearest to it, not only the exact double", "[unit]") {
    // 0.1 is not representable in binary. The float 0.1f is slightly above the double 0.1, so it must still pass a max of 0.1.
    ComponentBuilder<WaterVolume> builder("game.water_bounds", 1);
    builder.field<&WaterVolume::depth>("depth", 0.1f, Range{0.1, 0.1});
    const ComponentDescriptor descriptor = must_build(builder);

    WaterVolume water;
    const FieldValue exact = 0.1f;
    CHECK(descriptor.set_field(&water, "depth", exact).ok());
}

TEST_CASE("Duplicate type IDs are rejected and the catalog keeps the first registration", "[unit]") {
    ComponentCatalog catalog;
    REQUIRE(catalog.add(must_build(health_builder())).ok());

    ComponentBuilder<Stamina> clash("game.health", 1);
    clash.field<&Stamina::value>("value", 0);
    const auto added = catalog.add(must_build(clash));

    REQUIRE_FALSE(added.ok());
    CHECK(has_kind(added.error(), ReflectionErrorKind::DuplicateTypeId));
    CHECK(catalog.size() == 1);
    CHECK(catalog.find<Health>() != nullptr);
    CHECK(catalog.find<Stamina>() == nullptr);
}

TEST_CASE("The same C++ type cannot be registered under two type IDs", "[unit]") {
    ComponentCatalog catalog;
    REQUIRE(catalog.add(must_build(health_builder())).ok());

    ComponentBuilder<Health> second_name("game.health_alias", 3);
    second_name.field<&Health::hp>("hp", 100);
    const auto added = catalog.add(must_build(second_name));

    REQUIRE_FALSE(added.ok());
    CHECK(has_kind(added.error(), ReflectionErrorKind::DuplicateComponentType));
    CHECK(catalog.size() == 1);
}

TEST_CASE("Invalid defaults and constraints are detected when the descriptor is built", "[unit]") {
    const float quiet_nan = std::numeric_limits<float>::quiet_NaN();
    const float unbounded = std::numeric_limits<float>::infinity();

    auto nan_default = ComponentBuilder<Health>("game.bad", 1).field<&Health::armor>("armor", quiet_nan).build();
    REQUIRE_FALSE(nan_default.ok());
    CHECK(has_kind(nan_default.error(), ReflectionErrorKind::InvalidDefault));

    auto infinite_default = ComponentBuilder<Health>("game.bad", 1).field<&Health::armor>("armor", unbounded).build();
    REQUIRE_FALSE(infinite_default.ok());
    CHECK(has_kind(infinite_default.error(), ReflectionErrorKind::InvalidDefault));

    auto above_max = ComponentBuilder<Health>("game.bad", 1).field<&Health::hp>("hp", 5, Range{0.0, 4.0}).build();
    REQUIRE_FALSE(above_max.ok());
    CHECK(has_kind(above_max.error(), ReflectionErrorKind::InvalidDefault));

    auto below_min = ComponentBuilder<Health>("game.bad", 1).field<&Health::hp>("hp", -1, Range{0.0, 4.0}).build();
    REQUIRE_FALSE(below_min.ok());
    CHECK(has_kind(below_min.error(), ReflectionErrorKind::InvalidDefault));

    auto inverted = ComponentBuilder<Health>("game.bad", 1).field<&Health::hp>("hp", 1, Range{4.0, 0.0}).build();
    REQUIRE_FALSE(inverted.ok());
    CHECK(has_kind(inverted.error(), ReflectionErrorKind::InvalidConstraint));

    auto infinite_bound = ComponentBuilder<Health>("game.bad", 1).field<&Health::hp>("hp", 1, Range{0.0, unbounded}).build();
    REQUIRE_FALSE(infinite_bound.ok());
    CHECK(has_kind(infinite_bound.error(), ReflectionErrorKind::InvalidConstraint));

    auto range_on_bool = ComponentBuilder<Health>("game.bad", 1).field<&Health::invulnerable>("invulnerable", false, Range{0.0, 1.0}).build();
    REQUIRE_FALSE(range_on_bool.ok());
    CHECK(has_kind(range_on_bool.error(), ReflectionErrorKind::InvalidConstraint));

    // Boundary values are valid defaults.
    auto on_bounds = ComponentBuilder<Health>("game.ok", 1).field<&Health::armor>("armor", 1.0f, Range{0.0, 1.0}).build();
    CHECK(on_bounds.ok());
}

TEST_CASE("Malformed descriptors report type ID, schema version, and field name problems", "[unit]") {
    ComponentBuilder<Health> bad_id("Game.Health", 1);
    bad_id.field<&Health::hp>("hp", 1);
    auto bad_id_result = bad_id.build();
    REQUIRE_FALSE(bad_id_result.ok());
    CHECK(has_kind(bad_id_result.error(), ReflectionErrorKind::InvalidTypeId));

    auto zero_version = ComponentBuilder<Health>("game.health", 0).build();
    REQUIRE_FALSE(zero_version.ok());
    CHECK(has_kind(zero_version.error(), ReflectionErrorKind::InvalidSchemaVersion));

    ComponentBuilder<Health> bad_field("game.health", 1);
    bad_field.field<&Health::hp>("HP", 1);
    auto bad_field_result = bad_field.build();
    REQUIRE_FALSE(bad_field_result.ok());
    CHECK(has_kind(bad_field_result.error(), ReflectionErrorKind::InvalidFieldName));

    ComponentBuilder<Health> duplicate("game.health", 1);
    duplicate.field<&Health::hp>("value", 1);
    duplicate.field<&Health::armor>("value", 0.5f);
    auto duplicate_result = duplicate.build();
    REQUIRE_FALSE(duplicate_result.ok());
    CHECK(has_kind(duplicate_result.error(), ReflectionErrorKind::DuplicateFieldName));
}

TEST_CASE("A descriptor without accessors is reported as invalid", "[unit]") {
    FieldDescriptor field;
    field.name = "hp";
    field.default_value = std::int32_t{1};
    const std::type_index type(typeid(Health));

    ComponentDescriptor manual{"game.manual", 1, type, {field}};
    const auto errors = engine::scene::validate(manual);
    CHECK(has_kind(errors, ReflectionErrorKind::MissingAccessor));

    ComponentCatalog catalog;
    CHECK_FALSE(catalog.add(manual).ok());
    CHECK(catalog.size() == 0);
}

TEST_CASE("A component registered by game code appears in generic editor code without editor changes", "[unit]") {
    ComponentCatalog catalog;
    REQUIRE(catalog.add(must_build(health_builder())).ok());

    // Game code adds its own component. Nothing in describe_catalog changes.
    REQUIRE(catalog.add(must_build(water_builder())).ok());

    const std::vector<std::string> lines = describe_catalog(catalog);
    CHECK(lines[0] == "game.health v3");
    bool found_water = false;
    for (const std::string& line : lines) {
        if (line == "game.water_volume v1") {
            found_water = true;
        }
    }
    CHECK(found_water);

    // The generic path can also write the user component's fields through its descriptor.
    const ComponentDescriptor* water = catalog.find<WaterVolume>();
    REQUIRE(water != nullptr);
    WaterVolume volume;
    water->apply_defaults(&volume);
    CHECK(volume.depth == 2.0f);
    CHECK(volume.flow_direction[0] == 1.0f);

    const FieldValue deeper = 12.5f;
    REQUIRE(water->set_field(&volume, "depth", deeper).ok());
    CHECK(volume.depth == 12.5f);
}

TEST_CASE("Guid, vector, and integer fields round-trip through get and set", "[unit]") {
    ComponentBuilder<Placement> builder("game.placement", 2);
    builder.field<&Placement::position>("position", {0.0f, 0.0f, 0.0f});
    builder.field<&Placement::tint>("tint", {1.0f, 1.0f, 1.0f, 1.0f});
    builder.field<&Placement::prefab>("prefab", Guid{});
    const ComponentDescriptor descriptor = must_build(builder);

    Placement placement;
    const Guid prefab = Guid::generate();
    const FieldValue position = std::array<float, 3>{1.0f, 2.0f, 3.0f};
    const FieldValue prefab_value = prefab;
    REQUIRE(descriptor.set_field(&placement, "position", position).ok());
    REQUIRE(descriptor.set_field(&placement, "prefab", prefab_value).ok());
    CHECK(placement.position[2] == 3.0f);
    CHECK(placement.prefab == prefab);

    const auto read = descriptor.get_field(&placement, "prefab");
    REQUIRE(read.ok());
    CHECK(read.value() == prefab_value);

    const FieldValue too_small = std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
    CHECK_FALSE(descriptor.set_field(&placement, "position", too_small).ok());
}

TEST_CASE("A catalog descriptor pointer stays valid after later registrations", "[unit]") {
    ComponentCatalog catalog;
    REQUIRE(catalog.add(must_build(health_builder())).ok());
    const ComponentDescriptor* first = catalog.find<Health>();
    REQUIRE(first != nullptr);

    REQUIRE(catalog.add(must_build(water_builder())).ok());
    CHECK(catalog.find<Health>() == first);
    CHECK(first->type_id == "game.health");
}
