#include "engine/scene/reflection.hpp"

#include "engine/core/assert.hpp"

#include <cmath>
#include <cstdio>
#include <unordered_set>

namespace engine::scene {
namespace {

// One segment of a name: a lowercase letter, then lowercase letters, digits, or underscores.
bool is_name_segment(std::string_view segment) noexcept {
    if (segment.empty() || segment[0] < 'a' || segment[0] > 'z') {
        return false;
    }
    for (const char c : segment) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool is_float_kind(FieldKind kind) noexcept {
    return kind == FieldKind::Float32 || kind == FieldKind::Float32x2 || kind == FieldKind::Float32x3 ||
           kind == FieldKind::Float32x4;
}

bool has_numbers(FieldKind kind) noexcept {
    return kind != FieldKind::Bool && kind != FieldKind::Guid;
}

// Numeric components of a value, widened to double. Returns how many there are; bool and Guid have none.
std::size_t numeric_components(const FieldValue& value, std::array<double, 4>& out) {
    return std::visit(
        [&out](const auto& v) -> std::size_t {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, bool> || std::is_same_v<V, core::Guid>) {
                return 0;
            } else if constexpr (std::is_arithmetic_v<V>) {
                out[0] = static_cast<double>(v);
                return 1;
            } else {
                for (std::size_t i = 0; i < v.size(); ++i) {
                    out[i] = static_cast<double>(v[i]);
                }
                return v.size();
            }
        },
        value);
}

std::string number_text(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

// Compares in the field's own precision: a float bound such as 0.1 must accept the float 0.1f, which is not exactly 0.1.
double bound_for(double bound, bool floating) {
    return floating ? static_cast<double>(static_cast<float>(bound)) : bound;
}

}  // namespace

bool is_valid_type_id(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxTypeIdLength) {
        return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = name.find('.', start);
        const std::string_view segment = name.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (!is_name_segment(segment)) {
            return false;
        }
        if (dot == std::string_view::npos) {
            return true;
        }
        start = dot + 1;
    }
}

bool is_valid_field_name(std::string_view name) noexcept {
    return name.size() <= kMaxFieldNameLength && is_name_segment(name);
}

std::uint64_t type_id_hash(std::string_view name) noexcept {
    constexpr std::uint64_t kOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPrime = 0x100000001b3ULL;
    std::uint64_t hash = kOffsetBasis;
    for (const char c : name) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= kPrime;
    }
    return hash;
}

const FieldDescriptor* ComponentDescriptor::find_field(std::string_view name) const {
    for (const FieldDescriptor& field : fields) {
        if (std::string_view(field.name) == name) {
            return &field;
        }
    }
    return nullptr;
}

void ComponentDescriptor::apply_defaults(void* component) const {
    ENGINE_ASSERT(component != nullptr, "apply_defaults requires a component");
    for (const FieldDescriptor& field : fields) {
        field.set(component, field.default_value);
    }
}

core::Result<FieldValue, ReflectionError> ComponentDescriptor::get_field(const void* component, std::string_view name) const {
    ENGINE_ASSERT(component != nullptr, "get_field requires a component");
    const FieldDescriptor* field = find_field(name);
    if (field == nullptr) {
        return core::failure(ReflectionError{ReflectionErrorKind::UnknownField, std::string(name),
                                             "field is not declared on component " + type_id});
    }
    return core::success(field->get(component));
}

core::Result<core::Unit, ReflectionError> ComponentDescriptor::set_field(void* component, std::string_view name,
                                                                         const FieldValue& value) const {
    ENGINE_ASSERT(component != nullptr, "set_field requires a component");
    const FieldDescriptor* field = find_field(name);
    if (field == nullptr) {
        return core::failure(ReflectionError{ReflectionErrorKind::UnknownField, std::string(name),
                                             "field is not declared on component " + type_id});
    }
    if (std::optional<ReflectionError> problem = check_value(*field, value)) {
        return core::failure(std::move(*problem));
    }
    field->set(component, value);
    return core::success();
}

std::optional<ReflectionError> check_value(const FieldDescriptor& field, const FieldValue& value) {
    if (value.index() != field.default_value.index()) {
        return ReflectionError{ReflectionErrorKind::ValueTypeMismatch, field.name, "value kind does not match the field kind"};
    }

    std::array<double, 4> numbers{};
    const std::size_t count = numeric_components(value, numbers);
    const bool floating = is_float_kind(field.kind());
    for (std::size_t i = 0; i < count; ++i) {
        const double number = numbers[i];
        if (!std::isfinite(number)) {
            return ReflectionError{ReflectionErrorKind::NonFiniteValue, field.name, "value must be finite"};
        }
        if (field.range.min && number < bound_for(*field.range.min, floating)) {
            return ReflectionError{ReflectionErrorKind::ValueOutOfRange, field.name,
                                   "value " + number_text(number) + " is below the minimum " + number_text(*field.range.min)};
        }
        if (field.range.max && number > bound_for(*field.range.max, floating)) {
            return ReflectionError{ReflectionErrorKind::ValueOutOfRange, field.name,
                                   "value " + number_text(number) + " is above the maximum " + number_text(*field.range.max)};
        }
    }
    return std::nullopt;
}

std::vector<ReflectionError> validate(const ComponentDescriptor& descriptor) {
    std::vector<ReflectionError> errors;
    if (!is_valid_type_id(descriptor.type_id)) {
        errors.push_back({ReflectionErrorKind::InvalidTypeId, descriptor.type_id,
                          "type ID must be dot-separated segments that start with a lowercase letter and use lowercase "
                          "letters, digits, or underscores, at most 128 bytes"});
    }
    if (descriptor.schema_version == 0) {
        errors.push_back({ReflectionErrorKind::InvalidSchemaVersion, descriptor.type_id, "schema version must be at least 1"});
    }

    std::unordered_set<std::string> names;
    for (const FieldDescriptor& field : descriptor.fields) {
        if (!is_valid_field_name(field.name)) {
            errors.push_back({ReflectionErrorKind::InvalidFieldName, field.name,
                              "field name must start with a lowercase letter and use lowercase letters, digits, or "
                              "underscores, at most 64 bytes"});
        }
        if (!names.insert(field.name).second) {
            errors.push_back({ReflectionErrorKind::DuplicateFieldName, field.name, "field name is already used on this component"});
        }
        if (!field.get || !field.set) {
            errors.push_back({ReflectionErrorKind::MissingAccessor, field.name, "field has no getter or setter"});
        }

        const FieldKind kind = field.kind();
        if (!has_numbers(kind) && (field.range.min || field.range.max)) {
            errors.push_back({ReflectionErrorKind::InvalidConstraint, field.name, "a range does not apply to bool or Guid fields"});
        }
        if ((field.range.min && !std::isfinite(*field.range.min)) || (field.range.max && !std::isfinite(*field.range.max))) {
            errors.push_back({ReflectionErrorKind::InvalidConstraint, field.name, "range bounds must be finite"});
        }
        if (field.range.min && field.range.max && *field.range.min > *field.range.max) {
            errors.push_back({ReflectionErrorKind::InvalidConstraint, field.name, "range minimum is greater than its maximum"});
        }

        if (std::optional<ReflectionError> problem = check_value(field, field.default_value)) {
            errors.push_back({ReflectionErrorKind::InvalidDefault, field.name, "default is invalid: " + problem->message});
        }
    }
    return errors;
}

core::Result<core::Unit, std::vector<ReflectionError>> ComponentCatalog::add(ComponentDescriptor descriptor) {
    std::vector<ReflectionError> errors = validate(descriptor);

    if (by_name_.count(descriptor.type_id) != 0) {
        errors.push_back({ReflectionErrorKind::DuplicateTypeId, descriptor.type_id, "type ID is already registered"});
    }
    const std::uint64_t hash = type_id_hash(descriptor.type_id);
    if (const auto same_hash = by_hash_.find(hash); same_hash != by_hash_.end()) {
        const std::string& other = components_[same_hash->second].type_id;
        if (other != descriptor.type_id) {
            errors.push_back({ReflectionErrorKind::TypeIdHashCollision, descriptor.type_id,
                              "type ID hash collides with registered type ID " + other});
        }
    }
    if (by_type_.count(descriptor.type) != 0) {
        errors.push_back({ReflectionErrorKind::DuplicateComponentType, descriptor.type_id,
                          "C++ type is already registered under another type ID"});
    }

    if (!errors.empty()) {
        return core::failure(std::move(errors));
    }

    const std::size_t index = components_.size();
    by_name_.emplace(descriptor.type_id, index);
    by_hash_.emplace(hash, index);
    by_type_.emplace(descriptor.type, index);
    components_.push_back(std::move(descriptor));
    return core::success();
}

const ComponentDescriptor* ComponentCatalog::find(std::string_view type_id) const {
    const auto found = by_name_.find(std::string(type_id));
    return found == by_name_.end() ? nullptr : &components_[found->second];
}

const ComponentDescriptor* ComponentCatalog::find(std::type_index type) const {
    const auto found = by_type_.find(type);
    return found == by_type_.end() ? nullptr : &components_[found->second];
}

}  // namespace engine::scene
