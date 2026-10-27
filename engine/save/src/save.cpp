#include "engine/save/save.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/utf8.hpp"
#include "engine/world/partition.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#ifndef ENGINE_BUILD_VERSION
#define ENGINE_BUILD_VERSION "unknown"
#endif

namespace engine::save {
namespace {

constexpr std::string_view kChecksumPrefix = "crc32:";
constexpr std::string_view kPreviousSuffix = ".prev";
constexpr std::string_view kSaveSuffix = ".save";
constexpr std::size_t kMaxSlotLength = 64;

SaveError malformed(std::string message) {
    return SaveError{SaveErrorCode::Malformed, std::move(message)};
}

SaveError invalid_content(std::string message) {
    return SaveError{SaveErrorCode::InvalidContent, std::move(message)};
}

// IEEE 802.3 CRC-32, bit by bit. Saves are read once per load, so a table is not worth the code.
std::uint32_t crc32(std::string_view bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char byte : bytes) {
        crc ^= static_cast<unsigned char>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

std::string checksum_text(std::uint32_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(kChecksumPrefix);
    for (int shift = 28; shift >= 0; shift -= 4) {
        text.push_back(kDigits[(value >> shift) & 0xFu]);
    }
    return text;
}

// Parses "crc32:" plus eight lowercase or uppercase hex digits. Returns false for anything else.
bool parse_checksum(const std::string& text, std::uint32_t& value) {
    if (text.size() != kChecksumPrefix.size() + 8 || text.compare(0, kChecksumPrefix.size(), kChecksumPrefix) != 0) {
        return false;
    }
    value = 0;
    for (std::size_t i = kChecksumPrefix.size(); i < text.size(); ++i) {
        const char c = text[i];
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    return true;
}

// The compact text the checksum covers. nlohmann keeps object keys sorted, so the text does not depend on how the file is laid out.
// Nothing when a string in the body is not UTF-8, which only a damaged or foreign file can hold.
std::optional<std::string> body_text(const nlohmann::json& body) {
    try {
        return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

// Checks the envelope and returns the save id and the body. The format name and version are checked before the checksum, so an
// unknown file says what it is rather than that it is corrupt.
struct Envelope {
    core::Guid save_id;
    nlohmann::json body;
};

core::Result<Envelope, SaveError> open_envelope(std::string_view text) {
    const auto file = nlohmann::json::parse(text, nullptr, false);
    if (file.is_discarded() || !file.is_object()) {
        return core::failure(malformed("the save is not a JSON object"));
    }
    const auto format = file.find("format");
    if (format == file.end() || !format->is_string() || format->get_ref<const std::string&>() != kSaveFormat) {
        return core::failure(SaveError{SaveErrorCode::WrongFormat, "the file is not an engine.save file"});
    }
    const auto version = file.find("version");
    if (version == file.end() || !version->is_number_integer() || version->get<std::int64_t>() != kSaveFormatVersion) {
        return core::failure(SaveError{SaveErrorCode::UnsupportedVersion, "the save's format version is not supported by this build"});
    }
    for (const char* key : {"build", "save_id", "checksum", "body"}) {
        if (!file.contains(key)) {
            return core::failure(malformed(std::string("the save has no \"") + key + "\""));
        }
    }
    if (file.size() != 6) {
        return core::failure(malformed("the save has an unknown key"));
    }
    const auto& body = file.at("body");
    if (!body.is_object()) {
        return core::failure(malformed("\"body\" is not an object"));
    }
    const auto& save_id = file.at("save_id");
    const auto& checksum = file.at("checksum");
    if (!save_id.is_string() || !checksum.is_string()) {
        return core::failure(malformed("\"save_id\" and \"checksum\" must be strings"));
    }
    std::uint32_t expected = 0;
    if (!parse_checksum(checksum.get_ref<const std::string&>(), expected)) {
        return core::failure(malformed("\"checksum\" is not crc32 followed by eight hex digits"));
    }
    const auto text_of_body = body_text(body);
    if (!text_of_body) {
        return core::failure(malformed("the save holds text that is not UTF-8"));
    }
    if (crc32(*text_of_body) != expected) {
        return core::failure(SaveError{SaveErrorCode::ChecksumMismatch, "the save's contents do not match its checksum"});
    }
    auto id = core::Guid::parse(save_id.get_ref<const std::string&>());
    if (!id.ok() || id.value().is_nil()) {
        return core::failure(malformed("\"save_id\" is not a GUID"));
    }
    return core::success(Envelope{id.value(), body});
}

// Checks that an object has exactly the keys given.
bool has_exactly_keys(const nlohmann::json& object, std::initializer_list<const char*> keys) {
    if (!object.is_object() || object.size() != keys.size()) {
        return false;
    }
    for (const char* key : keys) {
        if (!object.contains(key)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string_view build_version() noexcept {
    return ENGINE_BUILD_VERSION;
}

scene::SceneDocument select_components(const scene::SceneDocument& document, const std::set<std::string>& selected) {
    scene::SceneDocument out = document;
    for (auto& entity : out.entities) {
        for (auto it = entity.components.begin(); it != entity.components.end();) {
            it = selected.count(it->first) != 0 ? std::next(it) : entity.components.erase(it);
        }
    }
    return out;
}

core::Result<std::string, SaveError> serialize_save(const SaveData& data) {
    if (data.save_id.is_nil()) {
        return core::failure(SaveError{SaveErrorCode::InvalidContent, "the save has a nil save_id"});
    }
    nlohmann::json body;
    const auto scene_json = nlohmann::json::parse(scene::serialize_scene(data.gameplay), nullptr, false);
    if (scene_json.is_discarded()) {
        return core::failure(invalid_content("the scene did not serialize"));
    }
    body["scene"] = scene_json;

    nlohmann::json scripts = nlohmann::json::array();
    for (const auto& snapshot : data.scripts) {
        if (!core::is_valid_utf8(snapshot.chunk)) {
            return core::failure(invalid_content("a script chunk name is not valid UTF-8"));
        }
        const auto state = nlohmann::json::parse(snapshot.state, nullptr, false);
        if (state.is_discarded() || !state.is_object()) {
            return core::failure(invalid_content("a script state is not a JSON object"));
        }
        scripts.push_back({{"entity", snapshot.entity.to_string()}, {"chunk", snapshot.chunk}, {"state", state}});
    }
    body["scripts"] = scripts;

    nlohmann::json cells = nlohmann::json::array();
    for (const auto& cell : data.world.cells) {
        cells.push_back(world::serialize_cell(cell));
    }
    nlohmann::json layer = nullptr;
    if (data.world.layer) {
        layer = world::serialize_layer(*data.world.layer);
    }
    body["world"] = {{"cells", cells}, {"layer", layer}};

    try {
        const std::string checksum = checksum_text(crc32(body.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict)));
        const nlohmann::json file = {
            {"format", std::string(kSaveFormat)},
            {"version", kSaveFormatVersion},
            {"build", {{"engine", std::string(build_version())}}},
            {"save_id", data.save_id.to_string()},
            {"checksum", checksum},
            {"body", body},
        };
        return core::success(file.dump(2, ' ', false, nlohmann::json::error_handler_t::strict) + "\n");
    } catch (const nlohmann::json::exception&) {
        return core::failure(invalid_content("a string is not valid UTF-8"));
    }
}

core::Result<SaveData, SaveError> parse_save(std::string_view text, const scene::ComponentCatalog& catalog) {
    auto envelope = open_envelope(text);
    if (!envelope.ok()) {
        return core::failure(envelope.error());
    }
    const nlohmann::json& body = envelope.value().body;
    if (!has_exactly_keys(body, {"scene", "scripts", "world"})) {
        return core::failure(malformed("\"body\" must hold exactly scene, scripts, and world"));
    }

    SaveData data;
    data.save_id = envelope.value().save_id;

    auto document = scene::parse_scene(body.at("scene").dump(), catalog);
    if (!document.ok()) {
        return core::failure(invalid_content("scene: " + document.error().message));
    }
    data.gameplay = std::move(document.value());

    const auto& scripts = body.at("scripts");
    if (!scripts.is_array()) {
        return core::failure(malformed("\"scripts\" is not an array"));
    }
    for (const auto& item : scripts) {
        if (!has_exactly_keys(item, {"entity", "chunk", "state"}) || !item.at("entity").is_string() || !item.at("chunk").is_string() ||
            !item.at("state").is_object()) {
            return core::failure(malformed("a script entry must hold entity (string), chunk (string), and state (object)"));
        }
        auto entity = core::Guid::parse(item.at("entity").get_ref<const std::string&>());
        if (!entity.ok()) {
            return core::failure(malformed("a script entity is not a GUID"));
        }
        data.scripts.push_back(ScriptSnapshot{entity.value(), item.at("chunk").get<std::string>(), item.at("state").dump()});
    }

    const auto& world_json = body.at("world");
    if (!has_exactly_keys(world_json, {"cells", "layer"}) || !world_json.at("cells").is_array()) {
        return core::failure(malformed("\"world\" must hold cells (array) and layer"));
    }
    for (const auto& cell_text : world_json.at("cells")) {
        if (!cell_text.is_string()) {
            return core::failure(malformed("a world cell is not a string"));
        }
        auto cell = world::parse_cell(cell_text.get_ref<const std::string&>());
        if (!cell.ok()) {
            return core::failure(invalid_content("world cell: " + cell.error()));
        }
        data.world.cells.push_back(std::move(cell.value()));
    }
    const auto& layer = world_json.at("layer");
    if (!layer.is_null()) {
        if (!layer.is_string()) {
            return core::failure(malformed("\"layer\" is neither null nor a string"));
        }
        auto parsed = world::parse_layer(layer.get_ref<const std::string&>());
        if (!parsed.ok()) {
            return core::failure(invalid_content("world layer: " + parsed.error()));
        }
        data.world.layer = std::move(parsed.value());
    }
    return core::success(std::move(data));
}

core::Result<core::Guid, SaveError> SaveSlots::write(std::string_view slot, SaveData data) const {
    auto current = file_for(slot, kSaveSuffix);
    if (!current.ok()) {
        return core::failure(current.error());
    }
    if (data.save_id.is_nil()) {
        data.save_id = core::Guid::generate();
    }
    auto text = serialize_save(data);
    if (!text.ok()) {
        return core::failure(text.error());
    }
    if (auto created = core::create_directories(directory_); !created.ok()) {
        return core::failure(SaveError{SaveErrorCode::Io, core::describe(created.error())});
    }

    // Keep the last good save. A slot that does not open (corrupt, or another format) is not copied over the previous save.
    auto existing = core::read_file(current.value());
    if (existing.ok() && open_envelope(existing.value()).ok()) {
        auto previous = file_for(slot, kPreviousSuffix);
        if (!previous.ok()) {
            return core::failure(previous.error());
        }
        if (auto kept = core::write_file_atomic(previous.value(), existing.value()); !kept.ok()) {
            return core::failure(SaveError{SaveErrorCode::Io, core::describe(kept.error())});
        }
    }
    if (auto written = core::write_file_atomic(current.value(), text.value()); !written.ok()) {
        return core::failure(SaveError{SaveErrorCode::Io, core::describe(written.error())});
    }
    return core::success(data.save_id);
}

core::Result<SaveData, SaveError> SaveSlots::read(std::string_view slot, const scene::ComponentCatalog& catalog) const {
    auto path = file_for(slot, kSaveSuffix);
    if (!path.ok()) {
        return core::failure(path.error());
    }
    auto text = core::read_file(path.value());
    if (!text.ok()) {
        if (text.error().code == core::IoErrorCode::NotFound) {
            return core::failure(SaveError{SaveErrorCode::NoSave, "the slot has no save"});
        }
        return core::failure(SaveError{SaveErrorCode::Io, core::describe(text.error())});
    }
    return parse_save(text.value(), catalog);
}

core::Result<SaveData, SaveError> SaveSlots::read_previous(std::string_view slot, const scene::ComponentCatalog& catalog) const {
    auto path = file_for(slot, kPreviousSuffix);
    if (!path.ok()) {
        return core::failure(path.error());
    }
    auto text = core::read_file(path.value());
    if (!text.ok()) {
        if (text.error().code == core::IoErrorCode::NotFound) {
            return core::failure(SaveError{SaveErrorCode::NoSave, "the slot has no previous save"});
        }
        return core::failure(SaveError{SaveErrorCode::Io, core::describe(text.error())});
    }
    return parse_save(text.value(), catalog);
}

bool SaveSlots::exists(std::string_view slot) const {
    auto path = file_for(slot, kSaveSuffix);
    return path.ok() && core::path_exists(path.value());
}

core::Result<core::Unit, SaveError> SaveSlots::remove(std::string_view slot) const {
    for (const std::string_view suffix : {kSaveSuffix, kPreviousSuffix}) {
        auto path = file_for(slot, suffix);
        if (!path.ok()) {
            return core::failure(path.error());
        }
        if (auto removed = core::remove_all(path.value()); !removed.ok()) {
            return core::failure(SaveError{SaveErrorCode::Io, core::describe(removed.error())});
        }
    }
    return core::success();
}

core::Result<core::Path, SaveError> SaveSlots::file_for(std::string_view slot, std::string_view suffix) const {
    if (slot.empty() || slot.size() > kMaxSlotLength) {
        return core::failure(SaveError{SaveErrorCode::InvalidSlot, "a slot name is 1 to 64 characters"});
    }
    for (const char c : slot) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) {
            return core::failure(SaveError{SaveErrorCode::InvalidSlot, "a slot name may hold only letters, digits, '_' and '-'"});
        }
    }
    auto path = directory_.join(std::string(slot) + std::string(suffix));
    if (!path.ok()) {
        return core::failure(SaveError{SaveErrorCode::InvalidSlot, "the slot does not make a valid file name"});
    }
    return core::success(std::move(path.value()));
}

}  // namespace engine::save
