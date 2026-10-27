#include "engine/ui/ui_layout.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <set>
#include <utility>

namespace engine::ui {

namespace {

using Json = nlohmann::json;

constexpr std::array<std::pair<const char*, Anchor>, 9> kAnchors{{
    {"TopLeft", Anchor::TopLeft},
    {"TopCentre", Anchor::TopCentre},
    {"TopRight", Anchor::TopRight},
    {"CentreLeft", Anchor::CentreLeft},
    {"Centre", Anchor::Centre},
    {"CentreRight", Anchor::CentreRight},
    {"BottomLeft", Anchor::BottomLeft},
    {"BottomCentre", Anchor::BottomCentre},
    {"BottomRight", Anchor::BottomRight},
}};

constexpr std::array<std::pair<const char*, ElementKind>, 5> kKinds{{
    {"panel", ElementKind::Panel},
    {"image", ElementKind::Image},
    {"text", ElementKind::Text},
    {"button", ElementKind::Button},
    {"slider", ElementKind::Slider},
}};

constexpr std::array<std::pair<const char*, TextAlign>, 3> kAligns{{
    {"start", TextAlign::Start},
    {"centre", TextAlign::Centre},
    {"end", TextAlign::End},
}};

template <typename T, std::size_t N>
const char* name_of(const std::array<std::pair<const char*, T>, N>& table, T value) {
    for (const auto& entry : table) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return table[0].first;
}

template <typename T, std::size_t N>
bool find_in(const std::array<std::pair<const char*, T>, N>& table, const std::string& name, T& out) {
    for (const auto& entry : table) {
        if (name == entry.first) {
            out = entry.second;
            return true;
        }
    }
    return false;
}

bool number(const Json& value, float& out) {
    if (!value.is_number()) {
        return false;
    }
    out = value.get<float>();
    return std::isfinite(out);
}

bool color(const Json& value, Rgba8& out) {
    if (!value.is_array() || value.size() != 4) {
        return false;
    }
    std::array<std::uint8_t, 4> parts{};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!value[i].is_number_integer() || value[i].get<int>() < 0 || value[i].get<int>() > 255) {
            return false;
        }
        parts[i] = static_cast<std::uint8_t>(value[i].get<int>());
    }
    out = Rgba8{parts[0], parts[1], parts[2], parts[3]};
    return true;
}

core::Result<LayoutElement, std::string> parse_element(const Json& json, std::size_t index) {
    const std::string where = "element " + std::to_string(index) + ": ";
    if (!json.is_object()) {
        return core::failure(where + "not an object");
    }
    static const std::set<std::string> known{"kind", "anchor", "x", "y", "w", "h", "color", "text_color", "label", "image", "text_size",
                                             "align", "value", "min", "max", "step", "visible", "enabled", "blocks_pointer", "on_click",
                                             "on_change"};
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (known.count(it.key()) == 0) {
            return core::failure(where + "unknown key '" + it.key() + "'");
        }
    }
    LayoutElement element;
    ElementSpec& spec = element.spec;
    if (!json.contains("kind") || !json["kind"].is_string() || !find_in(kKinds, json["kind"].get<std::string>(), spec.kind)) {
        return core::failure(where + "kind must be panel, image, text, button, or slider");
    }
    Anchor anchor = Anchor::TopLeft;
    if (json.contains("anchor") && (!json["anchor"].is_string() || !find_in(kAnchors, json["anchor"].get<std::string>(), anchor))) {
        return core::failure(where + "anchor is not one of the nine anchor names");
    }
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    for (const auto& [key, target] : std::initializer_list<std::pair<const char*, float*>>{{"x", &x}, {"y", &y}, {"w", &w}, {"h", &h}}) {
        if (json.contains(key) && !number(json[key], *target)) {
            return core::failure(where + key + " must be a finite number");
        }
    }
    if (!(w > 0.0f) || !(h > 0.0f)) {
        return core::failure(where + "w and h must be positive");
    }
    spec.placement = place(anchor, x, y, w, h);
    if (json.contains("color") && !color(json["color"], spec.colour)) {
        return core::failure(where + "color must be [r, g, b, a] with integers 0..255");
    }
    if (json.contains("text_color") && !color(json["text_color"], spec.text_colour)) {
        return core::failure(where + "text_color must be [r, g, b, a] with integers 0..255");
    }
    if (json.contains("label")) {
        if (!json["label"].is_string()) {
            return core::failure(where + "label must be text");
        }
        spec.label = json["label"].get<std::string>();
    }
    if (json.contains("image")) {
        if (!json["image"].is_string()) {
            return core::failure(where + "image must be text");
        }
        spec.image = json["image"].get<std::string>();
    }
    if (json.contains("align") && (!json["align"].is_string() || !find_in(kAligns, json["align"].get<std::string>(), spec.align))) {
        return core::failure(where + "align must be start, centre, or end");
    }
    for (const auto& [key, target] : std::initializer_list<std::pair<const char*, float*>>{
             {"text_size", &spec.text_size}, {"value", &spec.value}, {"min", &spec.minimum}, {"max", &spec.maximum}, {"step", &spec.step}}) {
        if (json.contains(key) && !number(json[key], *target)) {
            return core::failure(where + key + " must be a finite number");
        }
    }
    for (const auto& [key, target] : std::initializer_list<std::pair<const char*, bool*>>{
             {"visible", &spec.visible}, {"enabled", &spec.enabled}, {"blocks_pointer", &spec.blocks_pointer}}) {
        if (json.contains(key)) {
            if (!json[key].is_boolean()) {
                return core::failure(where + key + " must be true or false");
            }
            *target = json[key].get<bool>();
        }
    }
    for (const auto& [key, target] : std::initializer_list<std::pair<const char*, std::string*>>{{"on_click", &element.on_click},
                                                                                                  {"on_change", &element.on_change}}) {
        if (json.contains(key)) {
            if (!json[key].is_string()) {
                return core::failure(where + key + " must name a Lua function");
            }
            *target = json[key].get<std::string>();
        }
    }
    if (spec.kind == ElementKind::Slider && !(spec.maximum > spec.minimum)) {
        return core::failure(where + "a slider needs max greater than min");
    }
    return core::success(std::move(element));
}

}  // namespace

core::Result<UiLayout, std::string> parse_ui_layout(std::string_view text) {
    const Json document = Json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return core::failure(std::string("the UI layout is not a JSON object"));
    }
    for (auto it = document.begin(); it != document.end(); ++it) {
        if (it.key() != "format" && it.key() != "version" && it.key() != "reference" && it.key() != "elements") {
            return core::failure("the UI layout has an unknown key '" + it.key() + "'");
        }
    }
    if (!document.contains("format") || document["format"] != "engine.ui_layout") {
        return core::failure(std::string("the file is not an engine.ui_layout"));
    }
    if (!document.contains("version") || !document["version"].is_number_integer() || document["version"].get<int>() != 1) {
        return core::failure(std::string("the UI layout has an unsupported version"));
    }
    UiLayout layout;
    if (document.contains("reference")) {
        const Json& reference = document["reference"];
        if (!reference.is_array() || reference.size() != 2 || !number(reference[0], layout.reference_width) ||
            !number(reference[1], layout.reference_height) || !(layout.reference_width > 0.0f) || !(layout.reference_height > 0.0f)) {
            return core::failure(std::string("reference must be [width, height] with positive numbers"));
        }
    }
    if (!document.contains("elements") || !document["elements"].is_array()) {
        return core::failure(std::string("the UI layout needs an elements array"));
    }
    for (std::size_t i = 0; i < document["elements"].size(); ++i) {
        auto element = parse_element(document["elements"][i], i);
        if (!element.ok()) {
            return core::failure(element.error());
        }
        layout.elements.push_back(std::move(element).value());
    }
    return core::success(std::move(layout));
}

}  // namespace engine::ui
