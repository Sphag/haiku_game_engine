#include "engine/platform/action_profile.hpp"

#include "engine/core/file_io.hpp"
#include "engine/platform/input_names.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace engine::platform {
namespace {

using Json = nlohmann::json;

ActionError malformed(std::string message) {
    return ActionError{ActionErrorCode::MalformedProfile, std::move(message)};
}

// The member of an object, or nullptr when it is absent or the node is not an object.
const Json* member(const Json& object, const char* name) {
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}

// The shortest decimal text that reads back as the same float, so 0.2 is written as 0.2 and not 0.200000003.
double shortest_decimal(float value) {
    char buffer[32] = "0";
    for (int digits = 1; digits <= 9; ++digits) {
        std::snprintf(buffer, sizeof buffer, "%.*g", digits, static_cast<double>(value));
        if (std::strtof(buffer, nullptr) == value) {
            break;
        }
    }
    return std::strtod(buffer, nullptr);
}

Json binding_json(const Binding& binding) {
    Json out = Json::object();
    if (const auto* key = std::get_if<KeySource>(&binding.source)) {
        out["source"] = "key";
        out["key"] = std::string(key_name(key->key));
    } else if (const auto* button = std::get_if<MouseButtonSource>(&binding.source)) {
        out["source"] = "mouse_button";
        out["button"] = std::string(mouse_button_name(button->button));
    } else if (const auto* motion = std::get_if<MouseAxisSource>(&binding.source)) {
        out["source"] = "mouse_axis";
        out["axis"] = std::string(mouse_axis_name(motion->axis));
    } else if (const auto* pad_button = std::get_if<GamepadButtonSource>(&binding.source)) {
        out["source"] = "gamepad_button";
        out["button"] = std::string(gamepad_button_name(pad_button->button));
        if (pad_button->slot) {
            out["slot"] = *pad_button->slot;
        }
    } else if (const auto* pad_axis = std::get_if<GamepadAxisSource>(&binding.source)) {
        out["source"] = "gamepad_axis";
        out["axis"] = std::string(gamepad_axis_name(pad_axis->axis));
        if (pad_axis->slot) {
            out["slot"] = *pad_axis->slot;
        }
        out["dead_zone"] = shortest_decimal(binding.dead_zone);
    }
    out["scale"] = shortest_decimal(binding.scale);
    return out;
}

// Reads a string member. On failure, `problem` says which member is missing or not a string.
std::optional<std::string> string_member(const Json& node, const char* name, std::string& problem) {
    const Json* value = member(node, name);
    if (value == nullptr || !value->is_string()) {
        problem = std::string("binding needs a string '") + name + "'";
        return std::nullopt;
    }
    return value->get<std::string>();
}

// Reads an optional number member into `out`. Absent leaves `out` alone; a non-number sets `problem`.
bool number_member(const Json& node, const char* name, float& out, std::string& problem) {
    const Json* value = member(node, name);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_number()) {
        problem = std::string("'") + name + "' must be a number";
        return false;
    }
    out = static_cast<float>(value->get<double>());
    return true;
}

// Reads an optional gamepad slot. Absent means any pad. A negative or oversized value is rejected here; the range
// check below the slot limit is ActionMap's job.
bool slot_member(const Json& node, std::optional<std::uint32_t>& slot, std::string& problem) {
    const Json* value = member(node, "slot");
    if (value == nullptr) {
        return true;
    }
    if (!value->is_number_unsigned() || value->get<std::uint64_t>() > UINT32_MAX) {
        problem = "'slot' must be a non-negative integer";
        return false;
    }
    slot = static_cast<std::uint32_t>(value->get<std::uint64_t>());
    return true;
}

std::optional<Binding> parse_binding(const Json& node, std::string& problem) {
    if (!node.is_object()) {
        problem = "each binding must be an object";
        return std::nullopt;
    }
    const auto source = string_member(node, "source", problem);
    if (!source) {
        return std::nullopt;
    }

    Binding binding;
    if (!number_member(node, "scale", binding.scale, problem) ||
        !number_member(node, "dead_zone", binding.dead_zone, problem)) {
        return std::nullopt;
    }

    if (*source == "key") {
        const auto name = string_member(node, "key", problem);
        const auto key = name ? key_from_name(*name) : std::nullopt;
        if (!key) {
            problem = name ? "unknown key '" + *name + "'" : problem;
            return std::nullopt;
        }
        binding.source = KeySource{*key};
    } else if (*source == "mouse_button") {
        const auto name = string_member(node, "button", problem);
        const auto button = name ? mouse_button_from_name(*name) : std::nullopt;
        if (!button) {
            problem = name ? "unknown mouse button '" + *name + "'" : problem;
            return std::nullopt;
        }
        binding.source = MouseButtonSource{*button};
    } else if (*source == "mouse_axis") {
        const auto name = string_member(node, "axis", problem);
        const auto axis = name ? mouse_axis_from_name(*name) : std::nullopt;
        if (!axis) {
            problem = name ? "unknown mouse axis '" + *name + "'" : problem;
            return std::nullopt;
        }
        binding.source = MouseAxisSource{*axis};
    } else if (*source == "gamepad_button") {
        const auto name = string_member(node, "button", problem);
        const auto button = name ? gamepad_button_from_name(*name) : std::nullopt;
        if (!button) {
            problem = name ? "unknown gamepad button '" + *name + "'" : problem;
            return std::nullopt;
        }
        GamepadButtonSource pad_button{*button, std::nullopt};
        if (!slot_member(node, pad_button.slot, problem)) {
            return std::nullopt;
        }
        binding.source = pad_button;
    } else if (*source == "gamepad_axis") {
        const auto name = string_member(node, "axis", problem);
        const auto axis = name ? gamepad_axis_from_name(*name) : std::nullopt;
        if (!axis) {
            problem = name ? "unknown gamepad axis '" + *name + "'" : problem;
            return std::nullopt;
        }
        GamepadAxisSource pad_axis{*axis, std::nullopt};
        if (!slot_member(node, pad_axis.slot, problem)) {
            return std::nullopt;
        }
        binding.source = pad_axis;
    } else {
        problem = "unknown binding source '" + *source + "'";
        return std::nullopt;
    }
    return binding;
}

}  // namespace

std::string serialize_profile(const ProfileBindings& profile) {
    Json actions = Json::object();
    for (const auto& [name, bindings] : profile) {
        Json list = Json::array();
        for (const Binding& binding : bindings) {
            list.push_back(binding_json(binding));
        }
        actions[name] = std::move(list);
    }

    Json doc = Json::object();
    doc["format"] = std::string(kInputProfileFormat);
    doc["version"] = kInputProfileVersion;
    doc["actions"] = std::move(actions);
    return doc.dump(2) + "\n";
}

core::Result<ProfileBindings, ActionError> parse_profile(std::string_view text) {
    const Json doc = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        return core::failure(malformed("profile is not valid JSON"));
    }
    if (!doc.is_object()) {
        return core::failure(malformed("profile must be a JSON object"));
    }

    const Json* format = member(doc, "format");
    if (format == nullptr || !format->is_string() || format->get<std::string>() != std::string(kInputProfileFormat)) {
        return core::failure(malformed("not an input profile: expected format \"" + std::string(kInputProfileFormat) +
                                       "\""));
    }
    const Json* version = member(doc, "version");
    if (version == nullptr || !version->is_number_integer()) {
        return core::failure(malformed("profile has no integer version"));
    }
    if (version->get<std::int64_t>() != kInputProfileVersion) {
        return core::failure(ActionError{ActionErrorCode::UnsupportedVersion,
                                         "profile version " + std::to_string(version->get<std::int64_t>()) +
                                             " is not supported; this build reads version " +
                                             std::to_string(kInputProfileVersion)});
    }

    const Json* actions = member(doc, "actions");
    if (actions == nullptr || !actions->is_object()) {
        return core::failure(malformed("profile has no 'actions' object"));
    }

    ProfileBindings profile;
    for (auto action = actions->begin(); action != actions->end(); ++action) {
        const std::string& name = action.key();
        if (!action.value().is_array()) {
            return core::failure(malformed("action '" + name + "' must be a list of bindings"));
        }
        std::vector<Binding> bindings;
        for (const Json& node : action.value()) {
            std::string problem;
            auto binding = parse_binding(node, problem);
            if (!binding) {
                return core::failure(malformed("action '" + name + "': " + problem));
            }
            bindings.push_back(std::move(*binding));
        }
        profile[name] = std::move(bindings);
    }
    return core::success(std::move(profile));
}

core::Result<core::Unit, ActionError> save_profile(const core::Path& path, const ProfileBindings& profile) {
    const auto written = core::write_file_atomic(path, serialize_profile(profile));
    if (!written.ok()) {
        return core::failure(ActionError{ActionErrorCode::Io, core::describe(written.error())});
    }
    return core::success();
}

core::Result<ProfileBindings, ActionError> load_profile(const core::Path& path) {
    const auto text = core::read_text_file(path);
    if (!text.ok()) {
        return core::failure(ActionError{ActionErrorCode::Io, core::describe(text.error())});
    }
    return parse_profile(text.value());
}

}  // namespace engine::platform
