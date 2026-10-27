#include "engine/world/terrain_edit.hpp"

#include "engine/core/file_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace engine::world {
namespace {

constexpr std::size_t kNoSave = std::numeric_limits<std::size_t>::max();
constexpr char kMagic[4] = {'O', 'W', 'T', 'R'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderBytes = 32;

// Tile indices whose edge range holds sample `ix`: sample k * tile_cells belongs to tiles k - 1 and k.
std::uint32_t first_tile_of_sample(std::uint32_t ix, std::uint32_t tile_cells) {
    return ix > 0 ? (ix - 1) / tile_cells : 0;
}

std::uint32_t last_tile_of_sample(std::uint32_t ix, std::uint32_t tile_cells, std::uint32_t tiles) {
    return std::min(ix / tile_cells, tiles - 1);
}

float clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

void put_u32(std::string& out, std::uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xffu));
    }
}

std::uint32_t get_u32(std::string_view bytes, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
        v = (v << 8) | static_cast<std::uint8_t>(bytes[at + static_cast<std::size_t>(i)]);
    }
    return v;
}

std::uint32_t float_bits(float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return bits;
}

float bits_float(std::uint32_t bits) {
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

// Splat weights of one sample after a paint stamp. The sum S is kept; the painted layer moves toward 255 by t and the other layers scale
// to fill the rest. Rounding error goes to the largest channel, so the sum is exact.
std::array<std::uint8_t, kTerrainLayers> paint_splat(const std::array<std::uint8_t, kTerrainLayers>& before, std::uint32_t layer, float t) {
    std::array<std::uint8_t, kTerrainLayers> weights = before;
    int total = 0;
    for (std::uint8_t w : weights) {
        total += w;
    }
    if (total == 0) {
        weights[0] = 255;
        total = 255;
    }
    const float current = static_cast<float>(weights[layer]);
    const float want = current + (static_cast<float>(total) - current) * t;
    float others = 0.0f;
    for (std::uint32_t j = 0; j < kTerrainLayers; ++j) {
        if (j != layer) {
            others += static_cast<float>(weights[j]);
        }
    }

    std::array<int, kTerrainLayers> out{};
    for (std::uint32_t j = 0; j < kTerrainLayers; ++j) {
        if (j == layer) {
            out[j] = static_cast<int>(std::lround(want));
        } else if (others > 0.0f) {
            out[j] = static_cast<int>(std::lround(static_cast<float>(weights[j]) * (static_cast<float>(total) - want) / others));
        }
    }
    int sum = 0;
    std::uint32_t largest = 0;
    for (std::uint32_t j = 0; j < kTerrainLayers; ++j) {
        out[j] = std::clamp(out[j], 0, 255);
        sum += out[j];
        if (out[j] > out[largest]) {
            largest = j;
        }
    }
    out[largest] = std::clamp(out[largest] + (total - sum), 0, 255);

    std::array<std::uint8_t, kTerrainLayers> result{};
    for (std::uint32_t j = 0; j < kTerrainLayers; ++j) {
        result[j] = static_cast<std::uint8_t>(out[j]);
    }
    return result;
}

}  // namespace

core::Result<core::Unit, std::string> validate_brush(const Brush& brush) {
    if (!std::isfinite(brush.radius) || brush.radius <= 0.0f) {
        return core::failure(std::string("brush radius must be a positive number of metres"));
    }
    if (!std::isfinite(brush.strength) || brush.strength <= 0.0f || brush.strength > 1.0f) {
        return core::failure(std::string("brush strength must be in (0, 1]"));
    }
    if (!std::isfinite(brush.falloff) || brush.falloff < 0.0f || brush.falloff > 1.0f) {
        return core::failure(std::string("brush falloff must be in [0, 1]"));
    }
    if (!std::isfinite(brush.target_height)) {
        return core::failure(std::string("brush target height must be finite"));
    }
    if (brush.kind == BrushKind::Paint && brush.layer >= kTerrainLayers) {
        return core::failure(std::string("paint layer is out of range"));
    }
    return core::success(core::Unit{});
}

float brush_weight(float distance_over_radius, float falloff) noexcept {
    if (!(distance_over_radius < 1.0f)) {
        return 0.0f;
    }
    if (falloff <= 0.0f) {
        return 1.0f;
    }
    // The solid core is the inner (1 - falloff) of the radius; the weight eases across the band beyond it.
    const float t = clamp01((1.0f - std::max(distance_over_radius, 0.0f)) / falloff);
    return t * t * (3.0f - 2.0f * t);
}

SampleSpan brush_span(const Terrain& terrain, const Brush& brush, glm::vec2 centre_world) noexcept {
    SampleSpan span;
    if (!std::isfinite(centre_world.x) || !std::isfinite(centre_world.y) || !(brush.radius > 0.0f)) {
        return span;
    }
    const double cell = static_cast<double>(terrain.desc.cell_size);
    const double cx = static_cast<double>(centre_world.x) / cell;
    const double cz = static_cast<double>(centre_world.y) / cell;
    const double r = static_cast<double>(brush.radius) / cell;
    const double x_lo = std::floor(cx - r);
    const double x_hi = std::ceil(cx + r);
    const double z_lo = std::floor(cz - r);
    const double z_hi = std::ceil(cz + r);
    const double max_x = static_cast<double>(terrain.samples_x);
    const double max_z = static_cast<double>(terrain.samples_z);
    if (x_hi < 0.0 || z_hi < 0.0 || x_lo >= max_x || z_lo >= max_z) {
        return span;
    }
    span.x0 = static_cast<std::uint32_t>(std::max(x_lo, 0.0));
    span.z0 = static_cast<std::uint32_t>(std::max(z_lo, 0.0));
    span.x1 = static_cast<std::uint32_t>(std::min(x_hi + 1.0, max_x));
    span.z1 = static_cast<std::uint32_t>(std::min(z_hi + 1.0, max_z));
    return span;
}

std::vector<TileCoord> tiles_touching(const Terrain& terrain, const SampleSpan& span) {
    std::vector<TileCoord> tiles;
    if (span.empty()) {
        return tiles;
    }
    const std::uint32_t tc = terrain.desc.tile_cells;
    const std::uint32_t x_first = first_tile_of_sample(span.x0, tc);
    const std::uint32_t x_last = last_tile_of_sample(span.x1 - 1, tc, terrain.desc.tiles_x);
    const std::uint32_t z_first = first_tile_of_sample(span.z0, tc);
    const std::uint32_t z_last = last_tile_of_sample(span.z1 - 1, tc, terrain.desc.tiles_z);
    for (std::uint32_t z = z_first; z <= z_last; ++z) {
        for (std::uint32_t x = x_first; x <= x_last; ++x) {
            tiles.push_back(TileCoord{x, z});
        }
    }
    return tiles;
}

core::Result<SampleSpan, std::string> apply_brush_stamp(Terrain& terrain, const Brush& brush, glm::vec2 centre_world) {
    if (auto valid = validate_brush(brush); !valid.ok()) {
        return core::failure(std::string(valid.error()));
    }
    const SampleSpan span = brush_span(terrain, brush, centre_world);
    if (span.empty()) {
        return core::success(span);
    }

    // Every new value is computed from the state before the stamp, so smoothing reads its neighbours unchanged by this stamp.
    struct Pending {
        std::size_t index;
        float height;
        std::array<std::uint8_t, kTerrainLayers> splat;
    };
    std::vector<Pending> pending;
    const double cell = static_cast<double>(terrain.desc.cell_size);
    const double cx = static_cast<double>(centre_world.x);
    const double cz = static_cast<double>(centre_world.y);
    for (std::uint32_t iz = span.z0; iz < span.z1; ++iz) {
        for (std::uint32_t ix = span.x0; ix < span.x1; ++ix) {
            const double dx = static_cast<double>(ix) * cell - cx;
            const double dz = static_cast<double>(iz) * cell - cz;
            const float distance = static_cast<float>(std::hypot(dx, dz) / static_cast<double>(brush.radius));
            const float weight = brush_weight(distance, brush.falloff);
            if (weight <= 0.0f) {
                continue;
            }
            const std::size_t index = sample_index(terrain, ix, iz);
            const float h = terrain.heights[index];
            const float t = clamp01(brush.strength * weight);
            Pending p{index, h, terrain.splat[index]};
            switch (brush.kind) {
                case BrushKind::Raise:
                    p.height = h + kBrushHeightStep * brush.strength * weight;
                    break;
                case BrushKind::Lower:
                    p.height = h - kBrushHeightStep * brush.strength * weight;
                    break;
                case BrushKind::Smooth: {
                    float sum = 0.0f;
                    for (int oz = -1; oz <= 1; ++oz) {
                        for (int ox = -1; ox <= 1; ++ox) {
                            const std::uint32_t nx = static_cast<std::uint32_t>(
                                std::clamp(static_cast<std::int64_t>(ix) + ox, std::int64_t{0}, std::int64_t{terrain.samples_x} - 1));
                            const std::uint32_t nz = static_cast<std::uint32_t>(
                                std::clamp(static_cast<std::int64_t>(iz) + oz, std::int64_t{0}, std::int64_t{terrain.samples_z} - 1));
                            sum += terrain.heights[sample_index(terrain, nx, nz)];
                        }
                    }
                    p.height = h + (sum / 9.0f - h) * t;
                    break;
                }
                case BrushKind::Flatten:
                    p.height = h + (brush.target_height - h) * t;
                    break;
                case BrushKind::Paint:
                    p.splat = paint_splat(terrain.splat[index], brush.layer, t);
                    break;
            }
            pending.push_back(p);
        }
    }
    for (const Pending& p : pending) {
        terrain.heights[p.index] = p.height;
        terrain.splat[p.index] = p.splat;
    }
    return core::success(span);
}

core::Result<TerrainDocument, std::string> TerrainDocument::create(const TerrainDesc& desc) {
    auto terrain = create_terrain(desc);
    if (!terrain.ok()) {
        return core::failure(std::string(terrain.error()));
    }
    return core::success(TerrainDocument(std::move(terrain.value())));
}

TerrainDocument TerrainDocument::from_terrain(Terrain terrain) {
    return TerrainDocument(std::move(terrain));
}

TerrainDocument::TerrainDocument(Terrain terrain) : terrain_(std::move(terrain)) {
    revisions_.assign(static_cast<std::size_t>(terrain_.desc.tiles_x) * terrain_.desc.tiles_z, 0);
}

core::Result<core::Unit, std::string> TerrainDocument::begin_stroke(const Brush& brush) {
    if (stroke_) {
        return core::failure(std::string("a stroke is already open"));
    }
    if (auto valid = validate_brush(brush); !valid.ok()) {
        return core::failure(std::string(valid.error()));
    }
    stroke_.emplace(Stroke{brush, {}});
    return core::success(core::Unit{});
}

core::Result<core::Unit, std::string> TerrainDocument::stamp(glm::vec2 centre_world) {
    if (!stroke_) {
        return core::failure(std::string("no stroke is open"));
    }
    // Record each sample's value before the stroke at its first touch. The whole span is captured before the stamp changes it.
    const SampleSpan span = brush_span(terrain_, stroke_->brush, centre_world);
    for (std::uint32_t iz = span.z0; iz < span.z1; ++iz) {
        for (std::uint32_t ix = span.x0; ix < span.x1; ++ix) {
            const std::size_t index = sample_index(terrain_, ix, iz);
            if (stroke_->touched.count(index) == 0) {
                const float h = terrain_.heights[index];
                stroke_->touched.emplace(index, SampleChange{index, h, h, terrain_.splat[index], terrain_.splat[index]});
            }
        }
    }
    if (auto applied = apply_brush_stamp(terrain_, stroke_->brush, centre_world); !applied.ok()) {
        return core::failure(std::string(applied.error()));
    }
    return core::success(core::Unit{});
}

core::Result<bool, std::string> TerrainDocument::end_stroke() {
    if (!stroke_) {
        return core::failure(std::string("no stroke is open"));
    }
    Stroke stroke = std::move(*stroke_);
    stroke_.reset();

    Command command;
    std::vector<std::size_t> changed;
    for (auto& [index, change] : stroke.touched) {
        change.height_after = terrain_.heights[index];
        change.splat_after = terrain_.splat[index];
        if (change.height_after != change.height_before || change.splat_after != change.splat_before) {
            command.changes.push_back(change);
            changed.push_back(index);
        }
    }
    if (command.changes.empty()) {
        return core::success(false);
    }
    std::sort(command.changes.begin(), command.changes.end(),
              [](const SampleChange& a, const SampleChange& b) { return a.index < b.index; });

    // The history past the current point is discarded, and the saved point is lost if the history moved past it.
    history_.resize(history_pos_);
    if (saved_pos_ > history_pos_) {
        saved_pos_ = kNoSave;
    }
    history_.push_back(std::move(command));
    history_pos_ = history_.size();
    mark_samples(changed);
    return core::success(true);
}

core::Result<core::Unit, std::string> TerrainDocument::cancel_stroke() {
    if (!stroke_) {
        return core::failure(std::string("no stroke is open"));
    }
    std::vector<std::size_t> restored;
    for (const auto& [index, change] : stroke_->touched) {
        if (terrain_.heights[index] != change.height_before || terrain_.splat[index] != change.splat_before) {
            terrain_.heights[index] = change.height_before;
            terrain_.splat[index] = change.splat_before;
            restored.push_back(index);
        }
    }
    stroke_.reset();
    mark_samples(restored);
    return core::success(core::Unit{});
}

bool TerrainDocument::undo() {
    if (stroke_ || history_pos_ == 0) {
        return false;
    }
    --history_pos_;
    apply_command(history_[history_pos_], false);
    return true;
}

bool TerrainDocument::redo() {
    if (stroke_ || history_pos_ == history_.size()) {
        return false;
    }
    apply_command(history_[history_pos_], true);
    ++history_pos_;
    return true;
}

bool TerrainDocument::can_undo() const noexcept {
    return !stroke_ && history_pos_ > 0;
}

bool TerrainDocument::can_redo() const noexcept {
    return !stroke_ && history_pos_ < history_.size();
}

std::vector<TileCoord> TerrainDocument::take_dirty_tiles() {
    std::sort(dirty_.begin(), dirty_.end(), [](const TileCoord& a, const TileCoord& b) {
        return a.z != b.z ? a.z < b.z : a.x < b.x;
    });
    dirty_.erase(std::unique(dirty_.begin(), dirty_.end()), dirty_.end());
    std::vector<TileCoord> out = std::move(dirty_);
    dirty_.clear();
    return out;
}

std::uint64_t TerrainDocument::tile_revision(TileCoord tile) const {
    if (tile.x >= terrain_.desc.tiles_x || tile.z >= terrain_.desc.tiles_z) {
        return 0;
    }
    return revisions_[static_cast<std::size_t>(tile.z) * terrain_.desc.tiles_x + tile.x];
}

bool TerrainDocument::dirty() const noexcept {
    return stroke_.has_value() || saved_pos_ != history_pos_;
}

void TerrainDocument::mark_saved() noexcept {
    saved_pos_ = history_pos_;
}

void TerrainDocument::mark_samples(const std::vector<std::size_t>& indices) {
    std::vector<TileCoord> tiles;
    for (std::size_t index : indices) {
        const std::uint32_t ix = static_cast<std::uint32_t>(index % terrain_.samples_x);
        const std::uint32_t iz = static_cast<std::uint32_t>(index / terrain_.samples_x);
        // A sample's normal reads both neighbours, so the tile across an edge reads a changed sample that it does not own. The
        // footprint therefore grows by one sample each way, so that tile is rebuilt too.
        const SampleSpan one{ix > 0 ? ix - 1 : 0, iz > 0 ? iz - 1 : 0, std::min(ix + 2, terrain_.samples_x),
                             std::min(iz + 2, terrain_.samples_z)};
        for (const TileCoord& tile : tiles_touching(terrain_, one)) {
            tiles.push_back(tile);
        }
    }
    std::sort(tiles.begin(), tiles.end(), [](const TileCoord& a, const TileCoord& b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });
    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
    for (const TileCoord& tile : tiles) {
        ++revisions_[static_cast<std::size_t>(tile.z) * terrain_.desc.tiles_x + tile.x];
        dirty_.push_back(tile);
    }
}

void TerrainDocument::apply_command(const Command& command, bool use_after) {
    std::vector<std::size_t> indices;
    indices.reserve(command.changes.size());
    for (const SampleChange& change : command.changes) {
        terrain_.heights[change.index] = use_after ? change.height_after : change.height_before;
        terrain_.splat[change.index] = use_after ? change.splat_after : change.splat_before;
        indices.push_back(change.index);
    }
    mark_samples(indices);
}

std::string serialize_terrain(const Terrain& terrain) {
    std::string out;
    out.reserve(kHeaderBytes + terrain.heights.size() * 8);
    out.append(kMagic, sizeof kMagic);
    put_u32(out, kFormatVersion);
    put_u32(out, terrain.desc.tiles_x);
    put_u32(out, terrain.desc.tiles_z);
    put_u32(out, terrain.desc.tile_cells);
    put_u32(out, float_bits(terrain.desc.cell_size));
    put_u32(out, terrain.samples_x);
    put_u32(out, terrain.samples_z);
    for (float h : terrain.heights) {
        put_u32(out, float_bits(h));
    }
    for (const auto& weights : terrain.splat) {
        out.append(reinterpret_cast<const char*>(weights.data()), weights.size());
    }
    return out;
}

core::Result<Terrain, std::string> parse_terrain(std::string_view bytes) {
    if (bytes.size() < kHeaderBytes || std::memcmp(bytes.data(), kMagic, sizeof kMagic) != 0) {
        return core::failure(std::string("not a terrain file"));
    }
    if (get_u32(bytes, 4) != kFormatVersion) {
        return core::failure(std::string("unsupported terrain file version"));
    }
    TerrainDesc desc;
    desc.tiles_x = get_u32(bytes, 8);
    desc.tiles_z = get_u32(bytes, 12);
    desc.tile_cells = get_u32(bytes, 16);
    desc.cell_size = bits_float(get_u32(bytes, 20));
    if (auto valid = validate_terrain_desc(desc); !valid.ok()) {
        return core::failure(std::string(valid.error()));
    }
    const std::uint32_t samples_x = get_u32(bytes, 24);
    const std::uint32_t samples_z = get_u32(bytes, 28);
    if (samples_x != desc.tiles_x * desc.tile_cells + 1 || samples_z != desc.tiles_z * desc.tile_cells + 1) {
        return core::failure(std::string("terrain sample counts do not match the description"));
    }
    const std::size_t count = std::size_t{samples_x} * samples_z;
    if (bytes.size() != kHeaderBytes + count * 8) {
        return core::failure(std::string("terrain file size does not match its samples"));
    }

    Terrain terrain;
    terrain.desc = desc;
    terrain.samples_x = samples_x;
    terrain.samples_z = samples_z;
    terrain.heights.resize(count);
    terrain.splat.resize(count);
    std::size_t at = kHeaderBytes;
    for (std::size_t i = 0; i < count; ++i, at += 4) {
        const float h = bits_float(get_u32(bytes, at));
        if (!std::isfinite(h)) {
            return core::failure(std::string("terrain height is not finite"));
        }
        terrain.heights[i] = h;
    }
    for (std::size_t i = 0; i < count; ++i, at += kTerrainLayers) {
        for (std::uint32_t j = 0; j < kTerrainLayers; ++j) {
            terrain.splat[i][j] = static_cast<std::uint8_t>(bytes[at + j]);
        }
    }
    return core::success(std::move(terrain));
}

core::Result<core::Unit, std::string> save_terrain(const core::Path& path, const Terrain& terrain) {
    auto written = core::write_file_atomic(path, serialize_terrain(terrain));
    if (!written.ok()) {
        return core::failure(core::describe(written.error()));
    }
    return core::success(core::Unit{});
}

core::Result<Terrain, std::string> load_terrain(const core::Path& path) {
    auto bytes = core::read_file(path);
    if (!bytes.ok()) {
        return core::failure(core::describe(bytes.error()));
    }
    return parse_terrain(bytes.value());
}

}  // namespace engine::world
