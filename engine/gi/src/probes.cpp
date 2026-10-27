#include "engine/gi/probes.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace engine::gi {
namespace {

using core::Result;
using core::failure;
using core::success;

// Directions used by the sphere projection and the bake. Each run gives the same directions, so the bake is reproducible.
constexpr std::uint32_t kSkyProjectionSamples = 1024;
// The sun is a cone of about 3 degrees. Anything inside it gets the sun radiance.
const float kSunCosine = std::cos(glm::radians(3.0f));
// Offset along the hit normal before the visibility ray starts, so the ray does not start on the surface it leaves.
constexpr float kSurfaceEpsilon = 1e-3f;
// The first four bytes of a serialized volume.
constexpr char kMagic[4] = {'O', 'W', 'G', 'I'};

// Cosine-lobe convolution divided by pi (A_l / pi = 1, 2/3, 1/4 for bands 0, 1, 2). Same weights as the environment's diffuse SH.
float band_weight(std::size_t index) noexcept {
    if (index == 0) {
        return 1.0f;
    }
    return index < 4 ? 2.0f / 3.0f : 0.25f;
}

// The i-th of n directions on a Fibonacci sphere with +Y as the pole, so an elevation maps to d.y.
glm::vec3 fibonacci_direction(std::uint32_t index, std::uint32_t count) noexcept {
    const float z = 1.0f - (2.0f * static_cast<float>(index) + 1.0f) / static_cast<float>(count);
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const float phi = static_cast<float>(index) * 2.399963229728653f;
    return {r * std::cos(phi), z, r * std::sin(phi)};
}

// Projects a radiance function onto the nine diffuse coefficients: the integral of L * Y over the sphere, times the band weights.
template <typename Radiance>
Sh9 project_diffuse(std::uint32_t count, Radiance&& radiance) {
    Sh9 coefficients{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const glm::vec3 direction = fibonacci_direction(i, count);
        const glm::vec3 value = radiance(direction);
        const std::array<float, 9> basis = sh_basis(direction);
        for (std::size_t k = 0; k < 9; ++k) {
            coefficients[k] += value * basis[k];
        }
    }
    const float solid_angle = 4.0f * glm::pi<float>() / static_cast<float>(count);
    for (std::size_t k = 0; k < 9; ++k) {
        coefficients[k] *= solid_angle * band_weight(k);
    }
    return coefficients;
}

// A ray's first hit against one box.
struct BoxHit {
    float t = 0.0f;
    glm::vec3 normal{0.0f};  // outward, on the face the ray entered through
    std::size_t box = 0;
};

// Slab test of one ray against one box, within [0, max_t]. A ray that starts inside the box reports t = 0 with the normal opposite its
// direction.
bool intersect_box(const Box& box, const glm::vec3& origin, const glm::vec3& direction, float max_t, float& t_hit, glm::vec3& normal) noexcept {
    float t_enter = 0.0f;
    float t_exit = max_t;
    glm::vec3 entry_normal{0.0f};
    for (int axis = 0; axis < 3; ++axis) {
        const float d = direction[axis];
        if (std::fabs(d) < 1e-12f) {
            if (origin[axis] < box.min[axis] || origin[axis] > box.max[axis]) {
                return false;
            }
            continue;
        }
        const float inverse = 1.0f / d;
        float t_min_face = (box.min[axis] - origin[axis]) * inverse;
        float t_max_face = (box.max[axis] - origin[axis]) * inverse;
        glm::vec3 face_normal(0.0f);
        face_normal[axis] = -1.0f;  // entering through the min face
        if (t_min_face > t_max_face) {
            std::swap(t_min_face, t_max_face);
            face_normal[axis] = 1.0f;  // entering through the max face
        }
        if (t_min_face > t_enter) {
            t_enter = t_min_face;
            entry_normal = face_normal;
        }
        t_exit = std::min(t_exit, t_max_face);
        if (t_enter > t_exit) {
            return false;
        }
    }
    if (entry_normal == glm::vec3(0.0f)) {
        // The origin is inside the box, so the ray is blocked at once.
        t_hit = 0.0f;
        normal = -direction;
        return true;
    }
    t_hit = t_enter;
    normal = entry_normal;
    return true;
}

// The nearest box along the ray within max_t, if any.
bool closest_hit(const std::vector<Box>& boxes, const glm::vec3& origin, const glm::vec3& direction, float max_t, BoxHit& hit) noexcept {
    bool found = false;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        float t = 0.0f;
        glm::vec3 normal{0.0f};
        if (!intersect_box(boxes[i], origin, direction, max_t, t, normal)) {
            continue;
        }
        if (!found || t < hit.t) {
            hit.t = t;
            hit.normal = normal;
            hit.box = i;
            found = true;
        }
    }
    return found;
}

bool inside_any_box(const std::vector<Box>& boxes, const glm::vec3& point) noexcept {
    return std::any_of(boxes.begin(), boxes.end(), [&point](const Box& box) {
        return glm::all(glm::greaterThanEqual(point, box.min)) && glm::all(glm::lessThanEqual(point, box.max));
    });
}

glm::vec3 probe_position(const GridLayout& grid, std::size_t index) noexcept {
    const std::size_t x = index % grid.dims.x;
    const std::size_t y = (index / grid.dims.x) % grid.dims.y;
    const std::size_t z = index / (static_cast<std::size_t>(grid.dims.x) * grid.dims.y);
    return grid.origin + grid.spacing * glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}

// Radiance leaving a box face toward the probe: its emission, plus its albedo times the sky irradiance at the face if a ray along the
// normal reaches the sky.
glm::vec3 surface_radiance(const std::vector<Box>& boxes, const BoxHit& hit, const glm::vec3& point, const Sh9& sky_sh, float max_distance) noexcept {
    const Box& box = boxes[hit.box];
    const glm::vec3 start = point + hit.normal * kSurfaceEpsilon;
    BoxHit blocker;
    const bool visible = !closest_hit(boxes, start, hit.normal, max_distance, blocker);
    const glm::vec3 sky_irradiance = visible ? evaluate_irradiance(sky_sh, hit.normal) : glm::vec3(0.0f);
    return box.emitted + box.albedo * sky_irradiance;
}

// Shared fields of the hash, written in a fixed order.
class Fnv1a {
public:
    template <typename T>
    void add(const T& value) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        unsigned char bytes[sizeof(T)];
        std::memcpy(bytes, &value, sizeof(T));
        for (unsigned char byte : bytes) {
            state_ ^= byte;
            state_ *= 1099511628211ull;
        }
    }

    void add_vec3(const glm::vec3& value) noexcept {
        add(value.x);
        add(value.y);
        add(value.z);
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return state_; }

private:
    std::uint64_t state_ = 14695981039346656037ull;
};

bool finite_vec3(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool non_negative_vec3(const glm::vec3& value) noexcept {
    return finite_vec3(value) && glm::all(glm::greaterThanEqual(value, glm::vec3(0.0f)));
}

// Appends raw bytes of a trivially copyable value. The format is little-endian, which the static_assert below requires of the host.
template <typename T>
void put(std::string& out, const T& value) {
    char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.append(bytes, sizeof(T));
}

void put_vec3(std::string& out, const glm::vec3& value) {
    put(out, value.x);
    put(out, value.y);
    put(out, value.z);
}

// Reads what put() wrote, and fails on the first short read.
class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    template <typename T>
    bool take(T& value) noexcept {
        if (bytes_.size() - offset_ < sizeof(T)) {
            return false;
        }
        std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }

    bool take_vec3(glm::vec3& value) noexcept { return take(value.x) && take(value.y) && take(value.z); }

    [[nodiscard]] bool at_end() const noexcept { return offset_ == bytes_.size(); }

private:
    std::string_view bytes_;
    std::size_t offset_ = 0;
};

}  // namespace

static_assert(std::endian::native == std::endian::little, "the GI volume format is little-endian");

std::array<float, 9> sh_basis(const glm::vec3& d) noexcept {
    return {0.282095f,
            0.488603f * d.y,
            0.488603f * d.z,
            0.488603f * d.x,
            1.092548f * d.x * d.y,
            1.092548f * d.y * d.z,
            0.315392f * (3.0f * d.z * d.z - 1.0f),
            1.092548f * d.x * d.z,
            0.546274f * (d.x * d.x - d.y * d.y)};
}

glm::vec3 evaluate_irradiance(const Sh9& sh, const glm::vec3& normal) noexcept {
    const std::array<float, 9> basis = sh_basis(normal);
    glm::vec3 sum(0.0f);
    for (std::size_t i = 0; i < 9; ++i) {
        sum += sh[i] * basis[i];
    }
    return sum;
}

glm::vec3 sky_radiance(const Sky& sky, const glm::vec3& direction) noexcept {
    glm::vec3 radiance = direction.y < 0.0f ? sky.ground : glm::mix(sky.horizon, sky.zenith, direction.y);
    if (glm::dot(direction, sky.sun_direction) > kSunCosine) {
        radiance += sky.sun_radiance;
    }
    return radiance;
}

Sh9 project_sky(const Sky& sky) noexcept {
    return project_diffuse(kSkyProjectionSamples, [&sky](const glm::vec3& direction) { return sky_radiance(sky, direction); });
}

std::optional<std::string> validate_inputs(const BakeInputs& inputs) {
    const GridLayout& grid = inputs.grid;
    if (grid.dims.x == 0 || grid.dims.y == 0 || grid.dims.z == 0) {
        return std::string("the probe grid has no probes along an axis");
    }
    const std::uint64_t count = static_cast<std::uint64_t>(grid.dims.x) * grid.dims.y * grid.dims.z;
    if (count > kMaxProbes) {
        return std::string("the probe grid has more than ") + std::to_string(kMaxProbes) + " probes";
    }
    if (!finite_vec3(grid.origin)) {
        return std::string("the probe grid origin is not finite");
    }
    if (!std::isfinite(grid.spacing) || grid.spacing <= 0.0f) {
        return std::string("the probe spacing must be positive and finite");
    }
    if (inputs.samples == 0) {
        return std::string("the bake needs at least one sample per probe");
    }
    if (!std::isfinite(inputs.max_ray_distance) || inputs.max_ray_distance <= 0.0f) {
        return std::string("the maximum ray distance must be positive and finite");
    }
    const Sky& sky = inputs.sky;
    if (!non_negative_vec3(sky.zenith) || !non_negative_vec3(sky.horizon) || !non_negative_vec3(sky.ground) ||
        !non_negative_vec3(sky.sun_radiance)) {
        return std::string("sky colors must be finite and non-negative");
    }
    if (glm::length(sky.sun_direction) < 1e-6f || !finite_vec3(sky.sun_direction)) {
        return std::string("the sun direction must be a finite, non-zero vector");
    }
    for (const Box& box : inputs.boxes) {
        if (!finite_vec3(box.min) || !finite_vec3(box.max) || !glm::all(glm::lessThan(box.min, box.max))) {
            return std::string("every box needs finite bounds with min < max on each axis");
        }
        if (!non_negative_vec3(box.albedo) || !non_negative_vec3(box.emitted)) {
            return std::string("box albedo and emission must be finite and non-negative");
        }
    }
    return std::nullopt;
}

std::uint64_t hash_inputs(const BakeInputs& inputs) noexcept {
    Fnv1a hash;
    hash.add(kFormatVersion);
    hash.add_vec3(inputs.grid.origin);
    hash.add(inputs.grid.spacing);
    hash.add(inputs.grid.dims.x);
    hash.add(inputs.grid.dims.y);
    hash.add(inputs.grid.dims.z);
    hash.add_vec3(inputs.sky.zenith);
    hash.add_vec3(inputs.sky.horizon);
    hash.add_vec3(inputs.sky.ground);
    hash.add_vec3(inputs.sky.sun_direction);
    hash.add_vec3(inputs.sky.sun_radiance);
    hash.add(inputs.samples);
    hash.add(inputs.max_ray_distance);
    hash.add(static_cast<std::uint64_t>(inputs.boxes.size()));
    for (const Box& box : inputs.boxes) {
        hash.add_vec3(box.min);
        hash.add_vec3(box.max);
        hash.add_vec3(box.albedo);
        hash.add_vec3(box.emitted);
    }
    return hash.value();
}

GiVolume create_volume(const BakeInputs& inputs) {
    GiVolume volume;
    volume.source_hash = hash_inputs(inputs);
    volume.grid = inputs.grid;
    volume.probes.assign(static_cast<std::size_t>(inputs.grid.dims.x) * inputs.grid.dims.y * inputs.grid.dims.z, Probe{});
    return volume;
}

std::size_t bake_probes(GiVolume& volume, const BakeInputs& inputs, std::size_t budget) {
    if (!is_current(volume, inputs)) {
        return 0;
    }
    const Sh9 sky_sh = project_sky(inputs.sky);
    std::size_t baked = 0;
    for (std::size_t index = 0; index < volume.probes.size() && baked < budget; ++index) {
        Probe& probe = volume.probes[index];
        if (probe.state != ProbeState::Unbaked) {
            continue;
        }
        const glm::vec3 position = probe_position(inputs.grid, index);
        if (inside_any_box(inputs.boxes, position)) {
            probe.sh = Sh9{};
            probe.state = ProbeState::Blocked;
            ++baked;
            continue;
        }

        Sh9 coefficients{};
        for (std::uint32_t i = 0; i < inputs.samples; ++i) {
            const glm::vec3 direction = fibonacci_direction(i, inputs.samples);
            BoxHit hit;
            const glm::vec3 radiance = closest_hit(inputs.boxes, position, direction, inputs.max_ray_distance, hit)
                                           ? surface_radiance(inputs.boxes, hit, position + direction * hit.t, sky_sh, inputs.max_ray_distance)
                                           : sky_radiance(inputs.sky, direction);
            const std::array<float, 9> basis = sh_basis(direction);
            for (std::size_t k = 0; k < 9; ++k) {
                coefficients[k] += radiance * basis[k];
            }
        }
        const float solid_angle = 4.0f * glm::pi<float>() / static_cast<float>(inputs.samples);
        for (std::size_t k = 0; k < 9; ++k) {
            probe.sh[k] = coefficients[k] * (solid_angle * band_weight(k));
        }
        probe.state = ProbeState::Valid;
        ++baked;
    }
    return baked;
}

GiVolume bake_volume(const BakeInputs& inputs) {
    GiVolume volume = create_volume(inputs);
    // A fresh volume matches its inputs and has room for every probe, so the bake always finishes.
    static_cast<void>(bake_probes(volume, inputs, volume.probes.size()));
    return volume;
}

bool is_current(const GiVolume& volume, const BakeInputs& inputs) noexcept {
    const std::uint64_t count = static_cast<std::uint64_t>(inputs.grid.dims.x) * inputs.grid.dims.y * inputs.grid.dims.z;
    return volume.format_version == kFormatVersion && volume.source_hash == hash_inputs(inputs) && volume.grid.origin == inputs.grid.origin &&
           volume.grid.spacing == inputs.grid.spacing && volume.grid.dims == inputs.grid.dims && volume.probes.size() == count;
}

std::string serialize(const GiVolume& volume) {
    std::string out;
    out.append(kMagic, sizeof(kMagic));
    put(out, volume.format_version);
    put(out, volume.source_hash);
    put(out, volume.grid.dims.x);
    put(out, volume.grid.dims.y);
    put(out, volume.grid.dims.z);
    put_vec3(out, volume.grid.origin);
    put(out, volume.grid.spacing);
    put(out, static_cast<std::uint64_t>(volume.probes.size()));
    for (const Probe& probe : volume.probes) {
        put(out, static_cast<std::uint8_t>(probe.state));
        for (const glm::vec3& coefficient : probe.sh) {
            put_vec3(out, coefficient);
        }
    }
    return out;
}

Result<GiVolume, std::string> deserialize(std::string_view bytes) {
    if (bytes.size() < sizeof(kMagic) || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        return failure(std::string("not a GI volume: the magic is wrong"));
    }
    Reader reader(bytes.substr(sizeof(kMagic)));
    GiVolume volume;
    std::uint64_t count = 0;
    if (!reader.take(volume.format_version) || !reader.take(volume.source_hash) || !reader.take(volume.grid.dims.x) ||
        !reader.take(volume.grid.dims.y) || !reader.take(volume.grid.dims.z) || !reader.take_vec3(volume.grid.origin) ||
        !reader.take(volume.grid.spacing) || !reader.take(count)) {
        return failure(std::string("the GI volume header is truncated"));
    }
    if (volume.format_version != kFormatVersion) {
        return failure("GI volume format " + std::to_string(volume.format_version) + " is not supported; expected " +
                       std::to_string(kFormatVersion));
    }
    const std::uint64_t expected = static_cast<std::uint64_t>(volume.grid.dims.x) * volume.grid.dims.y * volume.grid.dims.z;
    if (expected == 0 || expected > kMaxProbes || count != expected) {
        return failure(std::string("the GI volume probe count does not match its grid"));
    }
    if (!finite_vec3(volume.grid.origin) || !std::isfinite(volume.grid.spacing) || volume.grid.spacing <= 0.0f) {
        return failure(std::string("the GI volume grid is not finite and positive"));
    }
    volume.probes.resize(static_cast<std::size_t>(count));
    for (Probe& probe : volume.probes) {
        std::uint8_t state = 0;
        if (!reader.take(state) || state > static_cast<std::uint8_t>(ProbeState::Blocked)) {
            return failure(std::string("a GI probe has an unknown state or the data is truncated"));
        }
        probe.state = static_cast<ProbeState>(state);
        for (glm::vec3& coefficient : probe.sh) {
            if (!reader.take_vec3(coefficient) || !finite_vec3(coefficient)) {
                return failure(std::string("a GI probe has a coefficient that is not finite or the data is truncated"));
            }
        }
    }
    if (!reader.at_end()) {
        return failure(std::string("the GI volume has data after its last probe"));
    }
    return success(std::move(volume));
}

IrradianceSample sample_irradiance(const GiVolume& volume, const glm::vec3& position, const glm::vec3& normal, const Sh9& fallback) noexcept {
    const glm::vec3 fallback_irradiance = evaluate_irradiance(fallback, normal);
    if (volume.probes.empty()) {
        return {fallback_irradiance, 0.0f};
    }
    const glm::vec3 dims(volume.grid.dims);
    const glm::vec3 grid = (position - volume.grid.origin) / volume.grid.spacing;
    if (glm::any(glm::lessThan(grid, glm::vec3(0.0f))) || glm::any(glm::greaterThan(grid, dims - glm::vec3(1.0f)))) {
        return {fallback_irradiance, 0.0f};
    }

    const glm::uvec3 low = glm::min(glm::uvec3(glm::floor(grid)), volume.grid.dims - glm::uvec3(1u));
    const glm::uvec3 high = glm::min(low + glm::uvec3(1u), volume.grid.dims - glm::uvec3(1u));
    const glm::vec3 fraction = grid - glm::vec3(low);

    glm::vec3 probe_sum(0.0f);
    float weight_sum = 0.0f;
    for (int corner = 0; corner < 8; ++corner) {
        const bool x_high = (corner & 1) != 0;
        const bool y_high = (corner & 2) != 0;
        const bool z_high = (corner & 4) != 0;
        const float weight = (x_high ? fraction.x : 1.0f - fraction.x) * (y_high ? fraction.y : 1.0f - fraction.y) *
                             (z_high ? fraction.z : 1.0f - fraction.z);
        if (weight <= 0.0f) {
            continue;
        }
        const std::size_t x = x_high ? high.x : low.x;
        const std::size_t y = y_high ? high.y : low.y;
        const std::size_t z = z_high ? high.z : low.z;
        const Probe& probe = volume.probes[x + volume.grid.dims.x * (y + static_cast<std::size_t>(volume.grid.dims.y) * z)];
        if (probe.state != ProbeState::Valid) {
            continue;
        }
        probe_sum += weight * evaluate_irradiance(probe.sh, normal);
        weight_sum += weight;
    }
    if (weight_sum <= 0.0f) {
        return {fallback_irradiance, 0.0f};
    }
    const float coverage = std::min(weight_sum, 1.0f);
    const glm::vec3 probe_irradiance = probe_sum / weight_sum;
    return {coverage * probe_irradiance + (1.0f - coverage) * fallback_irradiance, coverage};
}

glm::vec3 coverage_debug_color(const GiVolume& volume, const glm::vec3& position) noexcept {
    const glm::vec3 grid = (position - volume.grid.origin) / volume.grid.spacing;
    const glm::vec3 dims(volume.grid.dims);
    if (volume.probes.empty() || glm::any(glm::lessThan(grid, glm::vec3(-0.5f))) || glm::any(glm::greaterThan(grid, dims - glm::vec3(0.5f)))) {
        return glm::vec3(0.1f);
    }
    const glm::uvec3 nearest = glm::min(glm::uvec3(glm::max(glm::round(grid), glm::vec3(0.0f))), volume.grid.dims - glm::uvec3(1u));
    const Probe& probe = volume.probes[nearest.x + volume.grid.dims.x * (nearest.y + static_cast<std::size_t>(volume.grid.dims.y) * nearest.z)];
    switch (probe.state) {
    case ProbeState::Valid:
        return {0.0f, 1.0f, 0.0f};
    case ProbeState::Blocked:
        return {1.0f, 0.0f, 0.0f};
    case ProbeState::Unbaked:
        break;
    }
    return {1.0f, 1.0f, 0.0f};
}

}  // namespace engine::gi
