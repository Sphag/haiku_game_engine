#include "engine/editor/world_editor.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/file_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace engine::editor {
namespace {

namespace fs = std::filesystem;
using world::CellCoord;

constexpr std::string_view kDescFileName = "world.json";
constexpr std::string_view kDescFormat = "engine.world_desc";
constexpr int kDescVersion = 1;
constexpr std::string_view kStreamDirectoryName = ".stream";
constexpr unsigned kJobWorkers = 2;

// The grid description is not in any cell file, so the editor keeps it in world.json.
std::string serialize_desc(const world::WorldDesc& desc) {
    nlohmann::json document;
    document["format"] = std::string(kDescFormat);
    document["version"] = kDescVersion;
    document["origin"] = nlohmann::json::array({desc.origin.x, desc.origin.y});
    document["cell_size"] = desc.cell_size;
    return document.dump(2, ' ', false, nlohmann::json::error_handler_t::strict) + "\n";
}

core::Result<world::WorldDesc, std::string> parse_desc(std::string_view text) {
    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return core::failure(std::string("world.json is not a JSON object"));
    }
    const auto format = document.find("format");
    const auto version = document.find("version");
    const auto origin = document.find("origin");
    const auto cell_size = document.find("cell_size");
    if (format == document.end() || !format->is_string() || *format != std::string(kDescFormat)) {
        return core::failure(std::string("world.json has no engine.world_desc format"));
    }
    if (version == document.end() || !version->is_number_integer() || version->get<int>() != kDescVersion) {
        return core::failure(std::string("world.json has an unsupported version"));
    }
    if (origin == document.end() || !origin->is_array() || origin->size() != 2 || !(*origin)[0].is_number() ||
        !(*origin)[1].is_number()) {
        return core::failure(std::string("world.json has no two-number origin"));
    }
    if (cell_size == document.end() || !cell_size->is_number()) {
        return core::failure(std::string("world.json has no cell_size"));
    }
    world::WorldDesc desc;
    desc.origin = glm::vec2(static_cast<float>((*origin)[0].get<double>()), static_cast<float>((*origin)[1].get<double>()));
    desc.cell_size = static_cast<float>(cell_size->get<double>());
    if (auto valid = world::validate_world_desc(desc); !valid.ok()) {
        return core::failure(valid.error());
    }
    return core::success(desc);
}

// The cell files of a world directory, sorted by name so the load order is the same on every machine.
core::Result<std::vector<core::Path>, std::string> list_cell_files(const core::Path& directory) {
    std::error_code ec;
    std::vector<core::Path> files;
    fs::directory_iterator it(core::to_native_path(directory), ec);
    if (ec) {
        return core::failure("cannot read the world directory " + directory.str() + ": " + ec.message());
    }
    const fs::directory_iterator end{};
    for (; it != end; it.increment(ec)) {
        if (ec) {
            return core::failure("cannot list the world directory " + directory.str() + ": " + ec.message());
        }
        const fs::path& entry = it->path();
        if (entry.extension() != ".cell" || !fs::is_regular_file(entry, ec)) {
            continue;
        }
        auto path = core::Path::from_utf8(entry.string());
        if (!path.ok()) {
            return core::failure(std::string("a cell file name is not valid UTF-8"));
        }
        files.push_back(std::move(path.value()));
    }
    std::sort(files.begin(), files.end(), [](const core::Path& a, const core::Path& b) { return a.str() < b.str(); });
    return core::success(std::move(files));
}

}  // namespace

namespace {

// The stream cache lives inside the world directory. Its name is a valid component, so the join cannot fail.
core::Path stream_directory_of(const core::Path& directory) {
    auto stream = directory.join(kStreamDirectoryName);
    ENGINE_ASSERT(stream.ok(), "the stream directory name is a valid path component");
    return std::move(stream.value());
}

}  // namespace

WorldEditor::WorldEditor(world::World world, core::Path directory)
    : world_(std::move(world)),
      stream_directory_(stream_directory_of(directory)),
      directory_(std::move(directory)),
      jobs_(std::make_unique<core::JobSystem>(kJobWorkers)) {}

WorldEditor::WorldEditor(WorldEditor&&) noexcept = default;

WorldEditor::~WorldEditor() {
    // The manager goes before the job system it submits to, whatever the member order.
    stream_.reset();
}

core::Result<WorldEditor, std::string> WorldEditor::create(const world::WorldDesc& desc, const core::Path& directory) {
    auto made = world::create_world(desc);
    if (!made.ok()) {
        return core::failure(made.error());
    }
    WorldEditor editor(std::move(made.value()), directory);
    editor.reset_stream();
    return core::success(std::move(editor));
}

core::Result<WorldEditor, std::string> WorldEditor::open(const core::Path& directory) {
    const auto desc_path = directory.join(kDescFileName);
    if (!desc_path.ok()) {
        return core::failure(std::string("the world description file name is not a valid path"));
    }
    auto text = core::read_text_file(desc_path.value());
    if (!text.ok()) {
        return core::failure("cannot read world.json: " + core::describe(text.error()));
    }
    auto desc = parse_desc(text.value());
    if (!desc.ok()) {
        return core::failure(desc.error());
    }
    auto loaded = world::create_world(desc.value());
    if (!loaded.ok()) {
        return core::failure(loaded.error());
    }
    world::World world = std::move(loaded.value());

    auto files = list_cell_files(directory);
    if (!files.ok()) {
        return core::failure(files.error());
    }
    for (const auto& file : files.value()) {
        auto cell = world::load_cell(file);
        if (!cell.ok()) {
            return core::failure(cell.error());
        }
        world.cells.push_back(std::move(cell.value()));
    }
    std::sort(world.cells.begin(), world.cells.end(), [](const world::WorldCell& a, const world::WorldCell& b) {
        return a.coord.x != b.coord.x ? a.coord.x < b.coord.x : a.coord.z < b.coord.z;
    });

    const auto layer_path = directory.join(world::kLayerFileName);
    if (!layer_path.ok()) {
        return core::failure(std::string("the layer file name is not a valid path"));
    }
    if (core::path_exists(layer_path.value())) {
        auto layer = world::load_layer(layer_path.value());
        if (!layer.ok()) {
            return core::failure(layer.error());
        }
        world.layer = std::move(layer.value());
    }

    const std::vector<std::string> problems = world::validate_world(world);
    if (!problems.empty()) {
        return core::failure("the world is not valid: " + problems.front());
    }
    WorldEditor editor(std::move(world), directory);
    editor.reset_stream();
    return core::success(std::move(editor));
}

bool WorldEditor::dirty() const noexcept {
    if (world_.layer.dirty) {
        return true;
    }
    return std::any_of(world_.cells.begin(), world_.cells.end(), [](const world::WorldCell& cell) { return cell.dirty; });
}

core::Result<core::Unit, std::string> WorldEditor::add_object(world::WorldObject object, bool always_loaded) {
    auto added = world::add_object(world_, std::move(object), always_loaded);
    if (added.ok()) {
        stream_stale_ = true;
    }
    return added;
}

core::Result<core::Unit, std::string> WorldEditor::move_object(const core::Guid& id, glm::vec3 position) {
    auto moved = world::move_object(world_, id, position);
    if (moved.ok()) {
        stream_stale_ = true;
    }
    return moved;
}

core::Result<std::size_t, std::string> WorldEditor::save() {
    auto written = world::save_dirty(world_, directory_);
    if (!written.ok()) {
        return written;
    }
    const auto text = serialize_desc(world_.desc);
    const auto desc_path = directory_.join(kDescFileName);
    if (!desc_path.ok()) {
        return core::failure(std::string("the world description file name is not a valid path"));
    }
    if (auto done = core::write_file_atomic(desc_path.value(), text); !done.ok()) {
        return core::failure("cannot write world.json: " + core::describe(done.error()));
    }
    return written;
}

void WorldEditor::load_cell(CellCoord coord) {
    stream_->set_suppressed(coord, false);
    stream_->set_pinned(coord, true);
}

void WorldEditor::set_pinned(CellCoord coord, bool pinned) {
    stream_->set_pinned(coord, pinned);
}

void WorldEditor::unload_cell(CellCoord coord) {
    stream_->set_pinned(coord, false);
    stream_->set_suppressed(coord, true);
}

bool WorldEditor::pinned(CellCoord coord) const {
    return stream_->pinned(coord);
}

bool WorldEditor::suppressed(CellCoord coord) const {
    return stream_->suppressed(coord);
}

void WorldEditor::set_camera(glm::dvec2 xz) noexcept {
    camera_ = xz;
}

void WorldEditor::go_to_cell(CellCoord coord) {
    const world::CellBounds bounds = world::cell_bounds(world_.desc, coord);
    camera_ = glm::dvec2(bounds.min + bounds.max) * 0.5;
}

core::Result<core::Unit, std::string> WorldEditor::tick() {
    if (stream_stale_) {
        if (auto rebuilt = rebuild_stream(); !rebuilt.ok()) {
            return rebuilt;
        }
    }
    stream_->update(camera_);
    stream_->safe_point();
    return core::success(core::Unit{});
}

world::CellPhase WorldEditor::phase(CellCoord coord) const {
    return stream_->phase(coord);
}

const world::StreamingStats& WorldEditor::stats() const {
    return stream_->stats();
}

core::Result<core::Unit, std::string> WorldEditor::rebuild_stream() {
    // Keep every cell's residency override across the rebuild; the old manager reads files that are about to change, so it goes first.
    std::vector<std::pair<CellCoord, std::pair<bool, bool>>> overrides;
    if (stream_) {
        for (const auto& cell : world_.cells) {
            overrides.push_back({cell.coord, {stream_->pinned(cell.coord), stream_->suppressed(cell.coord)}});
        }
    }
    reset_stream();
    for (const auto& [coord, flags] : overrides) {
        stream_->set_pinned(coord, flags.first);
        stream_->set_suppressed(coord, flags.second);
    }

    if (auto made = core::create_directories(stream_directory_); !made.ok()) {
        return core::failure("cannot create the stream cache: " + core::describe(made.error()));
    }
    for (const auto& cell : world_.cells) {
        const auto path = stream_directory_.join(world::cell_file_name(cell));
        if (!path.ok()) {
            return core::failure(std::string("a cell file name is not a valid path"));
        }
        if (auto written = core::write_file_atomic(path.value(), world::serialize_cell(cell)); !written.ok()) {
            return core::failure("cannot write the stream cache: " + core::describe(written.error()));
        }
    }
    stream_stale_ = false;
    return core::success(core::Unit{});
}

void WorldEditor::reset_stream() {
    stream_.reset();
    stream_ = std::make_unique<world::StreamingManager>(*jobs_, stream_directory_, world_, window_, budget_);
}

}  // namespace engine::editor
