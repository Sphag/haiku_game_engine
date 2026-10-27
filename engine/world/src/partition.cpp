#include "engine/world/partition.hpp"

#include "engine/core/file_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace engine::world {
namespace {

// Largest cell index on either axis. It keeps cell_coord_at inside int32 and far outside any authored world.
constexpr float kMaxCellIndex = 1048576.0f;

[[nodiscard]] bool finite(glm::vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool index_in_range(const WorldDesc& desc, glm::vec3 position) noexcept {
    const glm::vec2 local = (glm::vec2(position.x, position.z) - desc.origin) / desc.cell_size;
    return std::fabs(local.x) <= kMaxCellIndex && std::fabs(local.y) <= kMaxCellIndex;
}

[[nodiscard]] bool one_line(std::string_view text) noexcept {
    return text.find('\n') == std::string_view::npos && text.find('\r') == std::string_view::npos;
}

[[nodiscard]] std::optional<std::size_t> find_cell(const World& world, CellCoord coord) {
    for (std::size_t i = 0; i < world.cells.size(); ++i) {
        if (world.cells[i].coord == coord) {
            return i;
        }
    }
    return std::nullopt;
}

void require_asset(WorldCell& cell, const core::Guid& asset) {
    if (std::find(cell.assets.begin(), cell.assets.end(), asset) == cell.assets.end()) {
        cell.assets.push_back(asset);
    }
}

[[nodiscard]] std::string format_float(float value) {
    // Nine significant digits round-trip every finite float exactly.
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return buffer;
}

// Reads one line of a text file, dropping a trailing carriage return. Returns false at the end of the text.
[[nodiscard]] bool next_line(std::string_view& text, std::string_view& line) {
    if (text.empty()) {
        return false;
    }
    const std::size_t end = text.find('\n');
    line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return true;
}

[[nodiscard]] std::string at_line(std::size_t number, const std::string& message) {
    return "line " + std::to_string(number) + ": " + message;
}

// Parses an object line: "object <guid> <asset> <x> <y> <z> <name>". The name is the rest of the line after one separating space.
[[nodiscard]] core::Result<WorldObject, std::string> parse_object(std::string_view line, std::size_t number) {
    std::istringstream stream{std::string(line)};
    std::string keyword;
    std::string id_text;
    std::string asset_text;
    WorldObject object;
    stream >> keyword >> id_text >> asset_text >> object.position.x >> object.position.y >> object.position.z;
    if (!stream) {
        return core::failure(at_line(number, "object needs a GUID, an asset GUID, and three coordinates"));
    }
    auto id = core::Guid::parse(id_text);
    if (!id.ok()) {
        return core::failure(at_line(number, "object GUID is not canonical: " + id_text));
    }
    auto asset = core::Guid::parse(asset_text);
    if (!asset.ok()) {
        return core::failure(at_line(number, "object asset GUID is not canonical: " + asset_text));
    }
    object.id = id.value();
    object.asset = asset.value();
    std::getline(stream, object.name);
    if (!object.name.empty() && object.name.front() == ' ') {
        object.name.erase(0, 1);
    }
    return core::success(std::move(object));
}

[[nodiscard]] core::Result<core::Guid, std::string> parse_guid_field(std::string_view line, std::size_t number, std::string_view what) {
    std::istringstream stream{std::string(line)};
    std::string keyword;
    std::string text;
    stream >> keyword >> text;
    auto guid = core::Guid::parse(text);
    if (!guid.ok()) {
        return core::failure(at_line(number, std::string(what) + " GUID is not canonical: " + text));
    }
    return core::success(guid.value());
}

// Parses the body shared by cells and the layer: object and ref lines until `end`. `allow_cell_lines` admits the cell's own
// "cell" and "asset" lines; the layer has neither.
struct Body {
    std::vector<WorldObject> objects;
    std::vector<core::Guid> assets;
};

[[nodiscard]] core::Result<Body, std::string> parse_body(std::string_view text, std::string_view format, bool is_cell,
                                                         std::size_t& line_number, CellCoord* coord, core::Guid* cell_id) {
    std::string_view rest = text;
    std::string_view line;
    if (!next_line(rest, line)) {
        return core::failure(std::string("empty file"));
    }
    ++line_number;
    const std::string expected = std::string(format) + " " + std::to_string(kWorldFormatVersion);
    if (line != expected) {
        return core::failure(at_line(line_number, "expected header '" + expected + "'"));
    }
    Body body;
    bool ended = false;
    while (next_line(rest, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        if (ended) {
            return core::failure(at_line(line_number, "text after 'end'"));
        }
        const std::string keyword(line.substr(0, line.find(' ')));
        if (keyword == "end") {
            ended = true;
        } else if (keyword == "cell" && is_cell) {
            std::istringstream stream{std::string(line)};
            std::string word;
            std::string id_text;
            stream >> word >> id_text >> coord->x >> coord->z;
            auto guid = core::Guid::parse(id_text);
            if (!stream || !guid.ok()) {
                return core::failure(at_line(line_number, "cell needs a canonical GUID and two integer coordinates"));
            }
            *cell_id = guid.value();
        } else if (keyword == "asset" && is_cell) {
            auto guid = parse_guid_field(line, line_number, "asset");
            if (!guid.ok()) {
                return core::failure(guid.error());
            }
            body.assets.push_back(guid.value());
        } else if (keyword == "object") {
            auto object = parse_object(line, line_number);
            if (!object.ok()) {
                return core::failure(object.error());
            }
            body.objects.push_back(std::move(object.value()));
        } else if (keyword == "ref") {
            if (body.objects.empty()) {
                return core::failure(at_line(line_number, "ref before any object"));
            }
            auto target = parse_guid_field(line, line_number, "reference");
            if (!target.ok()) {
                return core::failure(target.error());
            }
            body.objects.back().references.push_back(target.value());
        } else {
            return core::failure(at_line(line_number, "unknown keyword '" + keyword + "'"));
        }
    }
    if (!ended) {
        return core::failure(std::string("missing 'end' line; the file is truncated"));
    }
    return core::success(std::move(body));
}

void append_object(std::string& out, const WorldObject& object) {
    out += "object " + object.id.to_string() + " " + object.asset.to_string() + " " + format_float(object.position.x) + " " +
           format_float(object.position.y) + " " + format_float(object.position.z) + " " + object.name + "\n";
    for (const auto& target : object.references) {
        out += "ref " + target.to_string() + "\n";
    }
}

}  // namespace

core::Result<core::Unit, std::string> validate_world_desc(const WorldDesc& desc) {
    if (!std::isfinite(desc.origin.x) || !std::isfinite(desc.origin.y)) {
        return core::failure(std::string("world origin must be finite"));
    }
    if (!(desc.cell_size > 0.0f) || !std::isfinite(desc.cell_size) || desc.cell_size > kMaxCellSize) {
        return core::failure(std::string("world cell_size must be positive, finite, and at most " + format_float(kMaxCellSize)));
    }
    return core::success();
}

core::Result<World, std::string> create_world(const WorldDesc& desc) {
    if (auto valid = validate_world_desc(desc); !valid.ok()) {
        return core::failure(valid.error());
    }
    World world;
    world.desc = desc;
    return core::success(std::move(world));
}

CellCoord cell_coord_at(const WorldDesc& desc, glm::dvec2 xz) noexcept {
    // Double, so a camera far from the origin still lands in the cell it is in (US-050).
    const glm::dvec2 local = glm::floor((xz - glm::dvec2(desc.origin)) / static_cast<double>(desc.cell_size));
    return CellCoord{static_cast<std::int32_t>(local.x), static_cast<std::int32_t>(local.y)};
}

CellBounds cell_bounds(const WorldDesc& desc, CellCoord coord) noexcept {
    CellBounds bounds;
    bounds.min = desc.origin + glm::vec2(static_cast<float>(coord.x), static_cast<float>(coord.z)) * desc.cell_size;
    bounds.max = bounds.min + glm::vec2(desc.cell_size);
    return bounds;
}

std::size_t ensure_cell(World& world, CellCoord coord) {
    if (const auto found = find_cell(world, coord)) {
        return *found;
    }
    WorldCell cell;
    cell.id = core::Guid::generate();
    cell.coord = coord;
    cell.dirty = true;
    world.cells.push_back(std::move(cell));
    return world.cells.size() - 1;
}

core::Result<core::Unit, std::string> add_object(World& world, WorldObject object, bool always_loaded) {
    if (object.id.is_nil()) {
        return core::failure(std::string("object GUID must not be nil"));
    }
    if (object.asset.is_nil()) {
        return core::failure(std::string("object asset GUID must not be nil"));
    }
    if (!one_line(object.name)) {
        return core::failure(std::string("object name must be one line"));
    }
    if (!finite(object.position)) {
        return core::failure(std::string("object position must be finite"));
    }
    if (find_object(world, object.id)) {
        return core::failure("object " + object.id.to_string() + " already exists");
    }
    if (always_loaded) {
        world.layer.objects.push_back(std::move(object));
        world.layer.dirty = true;
        return core::success();
    }
    if (!index_in_range(world.desc, object.position)) {
        return core::failure(std::string("object position is outside the grid's index range"));
    }
    const std::size_t index = ensure_cell(world, cell_coord_at(world.desc, glm::dvec2(object.position.x, object.position.z)));
    WorldCell& cell = world.cells[index];
    require_asset(cell, object.asset);
    cell.objects.push_back(std::move(object));
    cell.dirty = true;
    return core::success();
}

std::optional<ObjectLocation> find_object(const World& world, const core::Guid& id) {
    for (std::size_t c = 0; c < world.cells.size(); ++c) {
        const auto& objects = world.cells[c].objects;
        for (std::size_t i = 0; i < objects.size(); ++i) {
            if (objects[i].id == id) {
                return ObjectLocation{false, c, i};
            }
        }
    }
    for (std::size_t i = 0; i < world.layer.objects.size(); ++i) {
        if (world.layer.objects[i].id == id) {
            return ObjectLocation{true, 0, i};
        }
    }
    return std::nullopt;
}

const WorldObject& object_at(const World& world, const ObjectLocation& location) {
    return location.in_layer ? world.layer.objects[location.index] : world.cells[location.cell].objects[location.index];
}

core::Result<core::Unit, std::string> move_object(World& world, const core::Guid& id, glm::vec3 position) {
    if (!finite(position)) {
        return core::failure(std::string("object position must be finite"));
    }
    const auto location = find_object(world, id);
    if (!location) {
        return core::failure("object " + id.to_string() + " does not exist");
    }
    if (location->in_layer) {
        world.layer.objects[location->index].position = position;
        world.layer.dirty = true;
        return core::success();
    }
    if (!index_in_range(world.desc, position)) {
        return core::failure(std::string("object position is outside the grid's index range"));
    }
    WorldCell& source = world.cells[location->cell];
    const CellCoord target = cell_coord_at(world.desc, glm::dvec2(position.x, position.z));
    if (target == source.coord) {
        source.objects[location->index].position = position;
        source.dirty = true;
        return core::success();
    }
    WorldObject moved = std::move(source.objects[location->index]);
    source.objects.erase(source.objects.begin() + static_cast<std::ptrdiff_t>(location->index));
    source.dirty = true;
    moved.position = position;
    const std::size_t destination = ensure_cell(world, target);
    WorldCell& cell = world.cells[destination];
    require_asset(cell, moved.asset);
    cell.objects.push_back(std::move(moved));
    cell.dirty = true;
    return core::success();
}

std::vector<std::string> validate_world(const World& world) {
    std::vector<std::string> problems;
    if (auto desc = validate_world_desc(world.desc); !desc.ok()) {
        problems.push_back(desc.error());
    }
    for (std::size_t c = 0; c < world.cells.size(); ++c) {
        const WorldCell& cell = world.cells[c];
        for (std::size_t other = 0; other < c; ++other) {
            if (world.cells[other].id == cell.id) {
                problems.push_back("cell GUID " + cell.id.to_string() + " is used twice");
            }
            if (world.cells[other].coord == cell.coord) {
                problems.push_back("cell coordinate (" + std::to_string(cell.coord.x) + ", " + std::to_string(cell.coord.z) +
                                   ") is used twice");
            }
        }
        for (const auto& object : cell.objects) {
            if (cell_coord_at(world.desc, glm::dvec2(object.position.x, object.position.z)) != cell.coord) {
                problems.push_back("object " + object.id.to_string() + " is stored in a cell that does not own its position");
            }
            if (std::find(cell.assets.begin(), cell.assets.end(), object.asset) == cell.assets.end()) {
                problems.push_back("cell " + cell.id.to_string() + " does not list the asset of object " + object.id.to_string());
            }
        }
    }
    std::vector<core::Guid> seen;
    const auto visit = [&](const WorldObject& object) {
        if (std::find(seen.begin(), seen.end(), object.id) != seen.end()) {
            problems.push_back("object GUID " + object.id.to_string() + " is used twice");
        }
        seen.push_back(object.id);
    };
    for (const auto& cell : world.cells) {
        for (const auto& object : cell.objects) {
            visit(object);
        }
    }
    for (const auto& object : world.layer.objects) {
        visit(object);
    }
    const auto check_references = [&](const WorldObject& object) {
        for (const auto& target : object.references) {
            if (!find_object(world, target)) {
                problems.push_back("object " + object.id.to_string() + " references missing object " + target.to_string());
            }
        }
    };
    for (const auto& cell : world.cells) {
        for (const auto& object : cell.objects) {
            check_references(object);
        }
    }
    for (const auto& object : world.layer.objects) {
        check_references(object);
    }
    return problems;
}

std::string serialize_cell(const WorldCell& cell) {
    std::string out = std::string(kCellFormat) + " " + std::to_string(kWorldFormatVersion) + "\n";
    out += "cell " + cell.id.to_string() + " " + std::to_string(cell.coord.x) + " " + std::to_string(cell.coord.z) + "\n";
    for (const auto& asset : cell.assets) {
        out += "asset " + asset.to_string() + "\n";
    }
    for (const auto& object : cell.objects) {
        append_object(out, object);
    }
    out += "end\n";
    return out;
}

core::Result<WorldCell, std::string> parse_cell(std::string_view text) {
    WorldCell cell;
    std::size_t line_number = 0;
    auto body = parse_body(text, kCellFormat, true, line_number, &cell.coord, &cell.id);
    if (!body.ok()) {
        return core::failure(body.error());
    }
    cell.assets = std::move(body.value().assets);
    cell.objects = std::move(body.value().objects);
    if (cell.id.is_nil()) {
        return core::failure(std::string("cell file has no 'cell' line"));
    }
    return core::success(std::move(cell));
}

std::string serialize_layer(const AlwaysLoadedLayer& layer) {
    std::string out = std::string(kLayerFormat) + " " + std::to_string(kWorldFormatVersion) + "\n";
    for (const auto& object : layer.objects) {
        append_object(out, object);
    }
    out += "end\n";
    return out;
}

core::Result<AlwaysLoadedLayer, std::string> parse_layer(std::string_view text) {
    CellCoord unused_coord;
    core::Guid unused_id;
    std::size_t line_number = 0;
    auto body = parse_body(text, kLayerFormat, false, line_number, &unused_coord, &unused_id);
    if (!body.ok()) {
        return core::failure(body.error());
    }
    AlwaysLoadedLayer layer;
    layer.objects = std::move(body.value().objects);
    return core::success(std::move(layer));
}

std::string cell_file_name(const WorldCell& cell) {
    return cell.id.to_string() + ".cell";
}

core::Result<std::size_t, std::string> save_dirty(World& world, const core::Path& directory) {
    if (auto made = core::create_directories(directory); !made.ok()) {
        return core::failure(core::describe(made.error()));
    }
    std::size_t written = 0;
    for (auto& cell : world.cells) {
        if (!cell.dirty) {
            continue;
        }
        auto path = directory.join(cell_file_name(cell));
        if (!path.ok()) {
            return core::failure(std::string("cell file name is not a valid path"));
        }
        if (auto saved = core::write_file_atomic(path.value(), serialize_cell(cell)); !saved.ok()) {
            return core::failure(core::describe(saved.error()));
        }
        cell.dirty = false;
        ++written;
    }
    if (world.layer.dirty) {
        auto path = directory.join(std::string(kLayerFileName));
        if (!path.ok()) {
            return core::failure(std::string("layer file name is not a valid path"));
        }
        if (auto saved = core::write_file_atomic(path.value(), serialize_layer(world.layer)); !saved.ok()) {
            return core::failure(core::describe(saved.error()));
        }
        world.layer.dirty = false;
        ++written;
    }
    return core::success(written);
}

core::Result<WorldCell, std::string> load_cell(const core::Path& file) {
    auto text = core::read_text_file(file);
    if (!text.ok()) {
        return core::failure(core::describe(text.error()));
    }
    return parse_cell(text.value());
}

core::Result<AlwaysLoadedLayer, std::string> load_layer(const core::Path& file) {
    auto text = core::read_text_file(file);
    if (!text.ok()) {
        return core::failure(core::describe(text.error()));
    }
    return parse_layer(text.value());
}

}  // namespace engine::world
