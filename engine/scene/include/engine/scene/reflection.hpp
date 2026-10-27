#pragma once

#include "engine/core/guid.hpp"
#include "engine/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace engine::scene {

inline constexpr std::size_t kMaxTypeIdLength = 128;
inline constexpr std::size_t kMaxFieldNameLength = 64;

// Kinds of value a reflected field can hold. A FieldKind equals the index of its alternative in FieldValue.
enum class FieldKind : std::uint8_t {
    Bool,
    Int32,
    UInt32,
    Float32,
    Float32x2,
    Float32x3,
    Float32x4,
    Guid,
};

using FieldValue = std::variant<bool, std::int32_t, std::uint32_t, float, std::array<float, 2>, std::array<float, 3>,
                                std::array<float, 4>, core::Guid>;

// Maps a C++ member type to its FieldKind. Any other type fails the static_assert, so a field cannot be reflected by accident.
template <typename T>
struct FieldTraits {
    static_assert(sizeof(T) == 0,
                  "unsupported reflected field type: use bool, std::int32_t, std::uint32_t, float, std::array<float, 2..4>, or core::Guid");
};

template <>
struct FieldTraits<bool> {
    static constexpr FieldKind kind = FieldKind::Bool;
};
template <>
struct FieldTraits<std::int32_t> {
    static constexpr FieldKind kind = FieldKind::Int32;
};
template <>
struct FieldTraits<std::uint32_t> {
    static constexpr FieldKind kind = FieldKind::UInt32;
};
template <>
struct FieldTraits<float> {
    static constexpr FieldKind kind = FieldKind::Float32;
};
template <>
struct FieldTraits<std::array<float, 2>> {
    static constexpr FieldKind kind = FieldKind::Float32x2;
};
template <>
struct FieldTraits<std::array<float, 3>> {
    static constexpr FieldKind kind = FieldKind::Float32x3;
};
template <>
struct FieldTraits<std::array<float, 4>> {
    static constexpr FieldKind kind = FieldKind::Float32x4;
};
template <>
struct FieldTraits<core::Guid> {
    static constexpr FieldKind kind = FieldKind::Guid;
};

// Stable identity of a component type. The name is written by the author ("engine.transform", "game.door_lock") and never
// comes from typeid or compiler output, so it survives rebuilds and platform changes. The hash is FNV-1a 64-bit of the name.
[[nodiscard]] bool is_valid_type_id(std::string_view name) noexcept;
[[nodiscard]] bool is_valid_field_name(std::string_view name) noexcept;
[[nodiscard]] std::uint64_t type_id_hash(std::string_view name) noexcept;

enum class ReflectionErrorKind {
    InvalidTypeId,         // type ID does not match the naming rule
    InvalidSchemaVersion,  // schema version must be at least 1
    InvalidFieldName,      // field name does not match the naming rule
    DuplicateFieldName,    // two fields of one component share a name
    MissingAccessor,       // a field has no getter or setter
    InvalidConstraint,     // range on a kind without numbers, a non-finite bound, or min above max
    InvalidDefault,        // default value is not finite or lies outside the range
    DuplicateTypeId,       // the catalog already has this type ID
    TypeIdHashCollision,   // two different type IDs share a hash
    DuplicateComponentType,  // the C++ type is already registered under another type ID
    UnknownField,          // set/get names a field that the component does not declare
    ValueTypeMismatch,     // value kind differs from the field kind
    NonFiniteValue,        // NaN or infinity in a float field
    ValueOutOfRange,       // value outside the field's range
};

// One problem found in a descriptor or a value. `subject` is the type ID or field name the problem belongs to.
struct ReflectionError {
    ReflectionErrorKind kind;
    std::string subject;
    std::string message;
};

// Inclusive bounds. For vector kinds every component must lie within them. Bool and Guid fields take no range.
struct Range {
    std::optional<double> min;
    std::optional<double> max;
};

struct FieldDescriptor {
    std::string name;
    FieldValue default_value;  // the kind of the field is the kind of this value
    Range range;

    // Type-erased access to the member. ComponentBuilder fills these in. The component pointer must be a live object of the
    // owning type; ComponentDescriptor::set_field and get_field check the name and the value, not the pointer.
    std::function<FieldValue(const void*)> get;
    std::function<void(void*, const FieldValue&)> set;

    [[nodiscard]] FieldKind kind() const noexcept {
        return static_cast<FieldKind>(default_value.index());
    }
};

// Everything the editor and the serializer need to know about one component type. Plain data: no virtual functions.
struct ComponentDescriptor {
    std::string type_id;
    std::uint32_t schema_version = 1;
    std::type_index type;  // the C++ type this descriptor reflects
    std::vector<FieldDescriptor> fields;

    [[nodiscard]] const FieldDescriptor* find_field(std::string_view name) const;

    // True when `C` is the type this descriptor reflects. Use it before passing a pointer to the functions below.
    template <typename C>
    [[nodiscard]] bool describes() const noexcept {
        return type == std::type_index(typeid(C));
    }

    // Writes every field's declared default into `component`.
    void apply_defaults(void* component) const;

    // Reads one field. Fails with UnknownField for a name the component does not declare.
    [[nodiscard]] core::Result<FieldValue, ReflectionError> get_field(const void* component, std::string_view name) const;

    // Writes one field after checking its kind and range. On failure the component is unchanged.
    [[nodiscard]] core::Result<core::Unit, ReflectionError> set_field(void* component, std::string_view name,
                                                                      const FieldValue& value) const;
};

// Checks one value against a field's kind, finiteness, and range. Returns nothing when the value is acceptable.
[[nodiscard]] std::optional<ReflectionError> check_value(const FieldDescriptor& field, const FieldValue& value);

// Checks a whole descriptor: type ID, schema version, field names and uniqueness, accessors, constraints, and defaults.
// An empty result means the descriptor is valid.
[[nodiscard]] std::vector<ReflectionError> validate(const ComponentDescriptor& descriptor);

namespace detail {
template <typename C, auto Member>
using MemberType = std::remove_cv_t<std::remove_reference_t<decltype(std::declval<C&>().*Member)>>;
}  // namespace detail

// Describes component C. Each field names a data member by pointer, so the getter and setter are generated and checked
// against the member's type. build() validates the result and returns every problem it finds.
template <typename C>
class ComponentBuilder {
public:
    ComponentBuilder(std::string_view type_id, std::uint32_t schema_version)
        : descriptor_{std::string(type_id), schema_version, std::type_index(typeid(C)), {}} {}

    template <auto Member>
    ComponentBuilder& field(std::string_view name, detail::MemberType<C, Member> default_value, Range range = {}) {
        using T = detail::MemberType<C, Member>;
        constexpr std::size_t index = static_cast<std::size_t>(FieldTraits<T>::kind);

        FieldDescriptor desc;
        desc.name = std::string(name);
        desc.default_value = FieldValue(std::in_place_index<index>, std::move(default_value));
        desc.range = range;
        desc.get = [](const void* component) -> FieldValue {
            return FieldValue(std::in_place_index<index>, static_cast<const C*>(component)->*Member);
        };
        desc.set = [](void* component, const FieldValue& value) {
            static_cast<C*>(component)->*Member = std::get<index>(value);
        };
        descriptor_.fields.push_back(std::move(desc));
        return *this;
    }

    [[nodiscard]] core::Result<ComponentDescriptor, std::vector<ReflectionError>> build() const {
        std::vector<ReflectionError> errors = validate(descriptor_);
        if (!errors.empty()) {
            return core::failure(std::move(errors));
        }
        return core::success(descriptor_);
    }

private:
    ComponentDescriptor descriptor_;
};

// Registry of component descriptors. Editor and serialization code iterate it without knowing component types, so a
// component registered by game code appears in tools without changes to those tools. Not thread-safe.
class ComponentCatalog {
public:
    // Validates the descriptor and adds it in registration order. On failure the catalog is unchanged.
    [[nodiscard]] core::Result<core::Unit, std::vector<ReflectionError>> add(ComponentDescriptor descriptor);

    [[nodiscard]] const ComponentDescriptor* find(std::string_view type_id) const;
    [[nodiscard]] const ComponentDescriptor* find(std::type_index type) const;

    template <typename C>
    [[nodiscard]] const ComponentDescriptor* find() const {
        return find(std::type_index(typeid(C)));
    }

    // Descriptors in registration order. Pointers and references stay valid while the catalog exists.
    [[nodiscard]] const std::deque<ComponentDescriptor>& components() const noexcept {
        return components_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return components_.size();
    }

private:
    std::deque<ComponentDescriptor> components_;
    std::unordered_map<std::string, std::size_t> by_name_;
    std::unordered_map<std::uint64_t, std::size_t> by_hash_;
    std::unordered_map<std::type_index, std::size_t> by_type_;
};

}  // namespace engine::scene
