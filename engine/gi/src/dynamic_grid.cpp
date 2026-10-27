#include "engine/gi/dynamic_grid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace engine::gi {

namespace {

// Bounds the grid origin, so a far-away eye cannot overflow the cell arithmetic. A cell at this range is still far beyond any world.
constexpr float kMaxOriginCells = 1.0e6f;

std::int32_t positive_mod(std::int32_t value, std::int32_t modulus) noexcept {
    const std::int32_t remainder = value % modulus;
    return remainder < 0 ? remainder + modulus : remainder;
}

bool finite(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

}  // namespace

std::optional<std::string> validate_desc(const DynamicGridDesc& desc) {
    if (desc.dims.x == 0 || desc.dims.y == 0 || desc.dims.z == 0) {
        return std::string("the probe grid has no probes along an axis");
    }
    const std::uint64_t count = static_cast<std::uint64_t>(desc.dims.x) * desc.dims.y * desc.dims.z;
    if (count > kMaxDynamicProbes) {
        return std::string("the probe grid has more than ") + std::to_string(kMaxDynamicProbes) + " probes";
    }
    if (!std::isfinite(desc.spacing) || desc.spacing <= 0.0f) {
        return std::string("the probe spacing must be positive and finite");
    }
    if (desc.rays_per_probe == 0 || desc.rays_per_probe > kMaxDynamicRays) {
        return std::string("rays per probe must be in [1, ") + std::to_string(kMaxDynamicRays) + "]";
    }
    if (desc.update_budget == 0 || desc.update_budget > count) {
        return std::string("the update budget must be in [1, ") + std::to_string(count) + "], the probe count";
    }
    if (desc.max_history == 0) {
        return std::string("the history must keep at least one sample");
    }
    if (desc.lighting_history == 0 || desc.lighting_history > desc.max_history) {
        return std::string("the lighting history must be in [1, the maximum history]");
    }
    return std::nullopt;
}

core::Result<DynamicGrid, std::string> DynamicGrid::create(const DynamicGridDesc& desc) {
    if (const std::optional<std::string> problem = validate_desc(desc)) {
        return core::failure(*problem);
    }
    return core::success(DynamicGrid(desc));
}

DynamicGrid::DynamicGrid(const DynamicGridDesc& desc) : desc_(desc) {
    // The grid starts centred on the origin. The first follow() moves it to the camera.
    const glm::ivec3 half(static_cast<std::int32_t>(desc_.dims.x / 2), static_cast<std::int32_t>(desc_.dims.y / 2),
                          static_cast<std::int32_t>(desc_.dims.z / 2));
    origin_ = -half;
    const std::size_t count = static_cast<std::size_t>(desc_.dims.x) * desc_.dims.y * desc_.dims.z;
    cells_.resize(count);
    samples_.assign(count, 0u);
    for (std::uint32_t slot = 0; slot < count; ++slot) {
        cells_[slot] = cell_of_slot(slot, origin_);
    }
}

glm::ivec3 DynamicGrid::cell_of_slot(std::uint32_t slot, const glm::ivec3& origin) const noexcept {
    // The slot's coordinates in the grid, then the cell in the window [origin, origin + dims) that has the same residue on each axis.
    const std::uint32_t x = slot % desc_.dims.x;
    const std::uint32_t y = (slot / desc_.dims.x) % desc_.dims.y;
    const std::uint32_t z = slot / (desc_.dims.x * desc_.dims.y);
    const glm::ivec3 coordinates(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z));
    glm::ivec3 result{0};
    for (int axis = 0; axis < 3; ++axis) {
        const auto dim = static_cast<std::int32_t>(desc_.dims[axis]);
        result[axis] = origin[axis] + positive_mod(coordinates[axis] - origin[axis], dim);
    }
    return result;
}

std::size_t DynamicGrid::follow(const glm::vec3& eye) {
    if (!finite(eye)) {
        return 0;
    }
    // The grid is centred on the eye's cell. Moving it by one cell changes only the plane of probes that the move brings in.
    const glm::vec3 scaled = glm::clamp(glm::floor(eye / desc_.spacing), glm::vec3(-kMaxOriginCells), glm::vec3(kMaxOriginCells));
    const glm::ivec3 eye_cell(static_cast<std::int32_t>(scaled.x), static_cast<std::int32_t>(scaled.y), static_cast<std::int32_t>(scaled.z));
    const glm::ivec3 half(static_cast<std::int32_t>(desc_.dims.x / 2), static_cast<std::int32_t>(desc_.dims.y / 2),
                          static_cast<std::int32_t>(desc_.dims.z / 2));
    const glm::ivec3 next_origin = eye_cell - half;
    if (next_origin == origin_) {
        return 0;
    }
    std::size_t changed = 0;
    for (std::uint32_t slot = 0; slot < cells_.size(); ++slot) {
        const glm::ivec3 next = cell_of_slot(slot, next_origin);
        if (next != cells_[slot]) {
            cells_[slot] = next;
            samples_[slot] = 0;
            ++changed;
        }
    }
    origin_ = next_origin;
    return changed;
}

void DynamicGrid::reset() noexcept {
    std::fill(samples_.begin(), samples_.end(), 0u);
}

std::size_t DynamicGrid::reset_region(const glm::vec3& min, const glm::vec3& max) {
    std::size_t inside = 0;
    for (std::uint32_t slot = 0; slot < cells_.size(); ++slot) {
        const glm::vec3 position = probe_position(slot);
        if (glm::all(glm::greaterThanEqual(position, min)) && glm::all(glm::lessThanEqual(position, max))) {
            samples_[slot] = 0;
            ++inside;
        }
    }
    return inside;
}

bool DynamicGrid::set_lighting(const DynamicLighting& lighting) {
    if (lighting.same_as(lighting_)) {
        return false;
    }
    lighting_ = lighting;
    for (std::uint32_t& count : samples_) {
        count = std::min(count, desc_.lighting_history);
    }
    return true;
}

std::vector<ProbeUpdate> DynamicGrid::plan_updates() {
    const auto total = static_cast<std::uint32_t>(cells_.size());
    std::vector<ProbeUpdate> plan;
    plan.reserve(desc_.update_budget);
    for (std::uint32_t i = 0; i < desc_.update_budget; ++i) {
        const std::uint32_t slot = (cursor_ + i) % total;
        // The first trace replaces whatever the history held (weight 1), and each later one is one more sample in the running average.
        const std::uint32_t averaged = std::min(samples_[slot], desc_.max_history - 1);
        ProbeUpdate update;
        update.slot = slot;
        update.weight = 1.0f / static_cast<float>(averaged + 1);
        update.cell = cells_[slot];
        plan.push_back(update);
    }
    cursor_ = (cursor_ + desc_.update_budget) % total;
    return plan;
}

void DynamicGrid::commit(const std::vector<ProbeUpdate>& updates) noexcept {
    for (const ProbeUpdate& update : updates) {
        // A probe that moved since it was planned has a new history, so its trace does not count for the cell it now holds.
        if (update.slot < cells_.size() && cells_[update.slot] == update.cell) {
            samples_[update.slot] = std::min(samples_[update.slot] + 1u, desc_.max_history);
        }
    }
}

std::uint32_t DynamicGrid::samples(std::uint32_t slot) const noexcept {
    return slot < samples_.size() ? samples_[slot] : 0u;
}

glm::ivec3 DynamicGrid::cell(std::uint32_t slot) const noexcept {
    return slot < cells_.size() ? cells_[slot] : glm::ivec3{0};
}

glm::vec3 DynamicGrid::probe_position(std::uint32_t slot) const noexcept {
    return (glm::vec3(cell(slot)) + 0.5f) * desc_.spacing;
}

GiVolume DynamicGrid::to_volume(const std::vector<Sh9>& history) const {
    GiVolume volume;
    volume.grid.origin = (glm::vec3(origin_) + 0.5f) * desc_.spacing;
    volume.grid.spacing = desc_.spacing;
    volume.grid.dims = desc_.dims;
    volume.probes.assign(cells_.size(), Probe{});
    if (history.size() != cells_.size()) {
        return volume;
    }
    for (std::uint32_t slot = 0; slot < cells_.size(); ++slot) {
        // The window is [origin, origin + dims), so every slot's cell maps to one probe of the volume, x fastest.
        const glm::ivec3 local = cells_[slot] - origin_;
        const auto ux = static_cast<std::size_t>(local.x);
        const auto uy = static_cast<std::size_t>(local.y);
        const auto uz = static_cast<std::size_t>(local.z);
        const std::size_t index = ux + desc_.dims.x * (uy + desc_.dims.y * uz);
        Probe& probe = volume.probes[index];
        probe.sh = history[slot];
        probe.state = samples_[slot] > 0 ? ProbeState::Valid : ProbeState::Unbaked;
    }
    return volume;
}

}  // namespace engine::gi
