#include "engine/benchmark/workload.hpp"

#include "engine/assets/content_hash.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <utility>

namespace engine::bench {
namespace {

using Json = nlohmann::json;

// World constants. The manifest's world block must match these, and a test checks it.
constexpr std::int64_t kSizeM = 8192;
constexpr std::int64_t kCellM = 256;
constexpr std::int64_t kGridCells = 32;
constexpr std::int64_t kWindowCells = 9;
constexpr std::uint64_t kWorldSeed = 0x5EED0001ull;

// Generator parameters that are not in the table. They are part of the workload, so changing one changes the digests and
// the expected file must be regenerated with them.
constexpr std::uint64_t kDefaultMeshLibrary = 64;   // unique meshes for scenes whose table names none
constexpr std::uint64_t kParticlesPerEmitter = 1000;
constexpr std::uint64_t kKeyframes = 60;            // two seconds at 30 Hz
constexpr std::int64_t kMm = 1000;                  // millimetres per metre
constexpr std::int64_t kOne = 1024;                 // fixed-point unit of the noise interpolation
constexpr std::int64_t kTerrainBaseMm = 40000;
constexpr std::int64_t kDrawDistanceMm = 1000000;   // the manifest's 1000 m object draw distance
constexpr std::int64_t kCentreMm = kSizeM * kMm / 2;

// One stream per component, so adding a component does not shift the others.
constexpr std::uint64_t kSaltMesh = 1;
constexpr std::uint64_t kSaltInstance = 2;
constexpr std::uint64_t kSaltCharacter = 3;
constexpr std::uint64_t kSaltLight = 4;
constexpr std::uint64_t kSaltParticle = 5;
constexpr std::uint64_t kSaltPhysics = 6;
constexpr std::uint64_t kSaltAudio = 7;
constexpr std::uint64_t kSaltTerrainLarge = 8;
constexpr std::uint64_t kSaltTerrainSmall = 9;

// The workload keys the table may name. Each maps to one recipe field, and the generated count is reported under `count`.
struct Recipe {
    std::uint64_t instances = 0;
    std::uint64_t meshes = kDefaultMeshLibrary;
    std::uint64_t characters = 0;
    std::uint64_t bones_per_character = 0;
    std::uint64_t particles = 0;
    std::uint64_t lights = 0;
    std::uint64_t shadowed_lights = 0;
    std::uint64_t suns = 0;
    std::uint64_t bodies = 0;
    std::uint64_t static_colliders = 0;
    std::uint64_t constraints = 0;
    std::uint64_t physics_characters = 0;
    std::uint64_t audio_emitters = 0;
};

struct TableKey {
    std::string_view key;
    std::uint64_t Recipe::*member;
};

constexpr TableKey kTableKeys[] = {
    {"renderable_instances", &Recipe::instances},
    {"descriptors", &Recipe::instances},
    {"unique_meshes", &Recipe::meshes},
    {"animated_characters", &Recipe::characters},
    {"bones_per_character", &Recipe::bones_per_character},
    {"live_gpu_particles", &Recipe::particles},
    {"live_particles", &Recipe::particles},
    {"local_lights", &Recipe::lights},
    {"shadowed_local_lights", &Recipe::shadowed_lights},
    {"directional_suns", &Recipe::suns},
    {"active_dynamic_bodies", &Recipe::bodies},
    {"static_colliders", &Recipe::static_colliders},
    {"constraints", &Recipe::constraints},
    {"characters", &Recipe::physics_characters},
    {"audio_emitters", &Recipe::audio_emitters},
};

// Workload keys that describe the route or the runtime rather than content. The generator does not build them, and the
// route story (US-076 to US-078) or the runtime measures them.
constexpr std::string_view kRuntimeKeys[] = {
    "raycasts_per_s",
    "shape_casts_per_s",
    "simulation_hz",
    "min_submitted_triangles_indexed_start_frame",
    "speed_m_per_s",
    "route_length_m",
    "resident_renderables",
    "terrain_layers",
};

std::uint64_t mix(std::uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// SplitMix64 over a seed, a salt, and an index. Integer only.
class Rng {
public:
    Rng(std::uint64_t seed, std::uint64_t salt, std::uint64_t index)
        : state_(mix(seed ^ mix(salt * 0x9E3779B97F4A7C15ull + index))) {}

    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ull;
        return mix(state_);
    }

    // Requires bound > 0.
    std::uint64_t below(std::uint64_t bound) {
        return next() % bound;
    }

private:
    std::uint64_t state_;
};

// Little-endian byte stream. Every component is serialised here and hashed as one buffer.
class Bytes {
public:
    void reserve(std::size_t n) {
        data_.reserve(n);
    }
    void u8(std::uint64_t value) {
        data_.push_back(static_cast<char>(static_cast<std::uint8_t>(value)));
    }
    void u32(std::uint64_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            u8(value >> shift);
        }
    }
    void i32(std::int64_t value) {
        u32(static_cast<std::uint64_t>(value));
    }
    [[nodiscard]] std::string digest() const {
        return assets::ContentHash::of(data_).to_hex();
    }

private:
    std::string data_;
};

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if (a % b != 0 && a < 0) {
        --q;
    }
    return q;
}

std::int64_t lattice_value(std::uint64_t world_seed, std::uint64_t salt, std::int64_t ix, std::int64_t iz) {
    const std::uint64_t h = mix(world_seed ^ mix(salt) ^ (static_cast<std::uint64_t>(ix) * 0x9E3779B97F4A7C15ull) ^
                                (static_cast<std::uint64_t>(iz) * 0xC2B2AE3D27D4EB4Full));
    return static_cast<std::int64_t>(h % 2049) - kOne;  // [-1024, 1024]
}

// Smoothstep on the fixed-point unit: 0 maps to 0 and kOne maps to kOne.
std::int64_t smooth(std::int64_t t) {
    return t * t * (3 * kOne - 2 * t) / (kOne * kOne);
}

// One octave of bilinear value noise with a smoothstep weight. The result is in millimetres, within +-amplitude.
std::int64_t octave(std::uint64_t world_seed, std::uint64_t salt, std::int64_t x, std::int64_t z, std::int64_t spacing,
                    std::int64_t amplitude) {
    const std::int64_t ix = floor_div(x, spacing);
    const std::int64_t iz = floor_div(z, spacing);
    const std::int64_t tx = smooth((x - ix * spacing) * kOne / spacing);
    const std::int64_t tz = smooth((z - iz * spacing) * kOne / spacing);
    const std::int64_t v00 = lattice_value(world_seed, salt, ix, iz);
    const std::int64_t v10 = lattice_value(world_seed, salt, ix + 1, iz);
    const std::int64_t v01 = lattice_value(world_seed, salt, ix, iz + 1);
    const std::int64_t v11 = lattice_value(world_seed, salt, ix + 1, iz + 1);
    const std::int64_t top = v00 + (v10 - v00) * tx / kOne;
    const std::int64_t bottom = v01 + (v11 - v01) * tx / kOne;
    const std::int64_t value = top + (bottom - top) * tz / kOne;
    return value * amplitude / kOne;
}

// One unique mesh before serialisation: vertices in millimetres (x, y, z per vertex, in grid order) and triangle indices.
struct MeshGeometry {
    std::uint64_t quads = 0;
    std::vector<std::int64_t> vertices_mm;
    std::vector<std::uint32_t> indices;
};

// The geometry of mesh `index`: a square grid of 24 to 96 quads per side, 1 m apart, with seeded heights. The density is set so
// the indexed start frame submits at least the 2 million triangles the fixture contract requires (US-076).
MeshGeometry build_mesh_geometry(std::uint64_t seed, std::uint64_t index) {
    Rng rng(seed, kSaltMesh, index);
    MeshGeometry mesh;
    mesh.quads = 24 + rng.below(73);
    const std::uint64_t side = mesh.quads + 1;
    for (std::uint64_t gz = 0; gz < side; ++gz) {
        for (std::uint64_t gx = 0; gx < side; ++gx) {
            mesh.vertices_mm.push_back(static_cast<std::int64_t>(gx) * kMm);
            mesh.vertices_mm.push_back(static_cast<std::int64_t>(rng.below(2000)));
            mesh.vertices_mm.push_back(static_cast<std::int64_t>(gz) * kMm);
        }
    }
    for (std::uint64_t gz = 0; gz < mesh.quads; ++gz) {
        for (std::uint64_t gx = 0; gx < mesh.quads; ++gx) {
            const std::uint64_t a = gz * side + gx;
            const std::uint64_t b = a + 1;
            const std::uint64_t c = a + side;
            const std::uint64_t d = c + 1;
            for (const std::uint64_t corner : {a, c, b, b, c, d}) {
                mesh.indices.push_back(static_cast<std::uint32_t>(corner));
            }
        }
    }
    return mesh;
}

struct MeshSet {
    std::vector<std::uint64_t> triangles;   // per mesh
    std::string sha256;
};

MeshSet build_meshes(std::uint64_t seed, std::uint64_t count) {
    Bytes out;
    MeshSet set;
    for (std::uint64_t i = 0; i < count; ++i) {
        const MeshGeometry mesh = build_mesh_geometry(seed, i);
        out.u32(mesh.quads);
        for (const std::int64_t value : mesh.vertices_mm) {
            out.i32(value);
        }
        for (const std::uint32_t index : mesh.indices) {
            out.u32(index);
        }
        set.triangles.push_back(2 * mesh.quads * mesh.quads);
    }
    set.sha256 = out.digest();
    return set;
}

struct InstanceSet {
    std::uint64_t triangles = 0;             // triangles of every instance together
    std::uint64_t within_draw_distance = 0;  // instances within 1000 m of the world centre
    std::string sha256;
};

// One instance descriptor, in millimetres (position), millidegrees (yaw), and permille (scale).
struct InstanceRecord {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
    std::uint64_t mesh = 0;
    std::uint64_t yaw = 0;
    std::uint64_t scale = 0;
};

// Instance descriptors: a position on the terrain, a mesh, a yaw, and a scale. Placement is uniform over the world.
std::vector<InstanceRecord> place_instances(std::uint64_t seed, std::uint64_t count, std::uint64_t mesh_count) {
    std::vector<InstanceRecord> records;
    records.reserve(static_cast<std::size_t>(count));
    Rng rng(seed, kSaltInstance, 0);
    for (std::uint64_t k = 0; k < count; ++k) {
        InstanceRecord record;
        record.x = static_cast<std::int64_t>(rng.below(kSizeM * kMm));
        record.z = static_cast<std::int64_t>(rng.below(kSizeM * kMm));
        record.y = terrain_height_mm(kWorldSeed, record.x, record.z);
        record.mesh = rng.below(mesh_count);
        record.yaw = rng.below(360000);       // millidegrees
        record.scale = 800 + rng.below(400);  // permille
        records.push_back(record);
    }
    return records;
}

InstanceSet build_instances(const std::vector<InstanceRecord>& records, const MeshSet& meshes) {
    Bytes out;
    out.reserve(records.size() * 24);
    InstanceSet set;
    for (const InstanceRecord& record : records) {
        out.i32(record.x);
        out.i32(record.y);
        out.i32(record.z);
        out.u32(record.mesh);
        out.u32(record.yaw);
        out.u32(record.scale);
        set.triangles += meshes.triangles[record.mesh];
        const std::int64_t dx = record.x - kCentreMm;
        const std::int64_t dz = record.z - kCentreMm;
        if (dx * dx + dz * dz <= kDrawDistanceMm * kDrawDistanceMm) {
            ++set.within_draw_distance;
        }
    }
    set.sha256 = out.digest();
    return set;
}

struct CharacterSet {
    std::uint64_t bones = 0;
    std::uint64_t keyframes = 0;
    std::string sha256;
};

// Animated characters: a binary-tree skeleton and a looping clip per character. Each bone rotates by a triangle wave with its
// own amplitude and phase, in millidegrees, so a clip is a whole number of cycles.
CharacterSet build_characters(std::uint64_t seed, std::uint64_t count, std::uint64_t bones_per_character) {
    Bytes out;
    CharacterSet set;
    Rng rng(seed, kSaltCharacter, 0);
    for (std::uint64_t c = 0; c < count; ++c) {
        std::vector<std::uint64_t> amplitude(bones_per_character);
        std::vector<std::uint64_t> phase(bones_per_character);
        for (std::uint64_t b = 0; b < bones_per_character; ++b) {
            const std::uint64_t parent = b == 0 ? 0xFFFFFFFFull : (b - 1) / 2;
            out.u32(parent);
            out.i32(static_cast<std::int64_t>(rng.below(400)) - 200);
            out.i32(static_cast<std::int64_t>(rng.below(400)) - 200);
            out.i32(static_cast<std::int64_t>(rng.below(400)) - 200);
            amplitude[b] = rng.below(30000);
            phase[b] = rng.below(360000);
            out.u32(amplitude[b]);
            out.u32(phase[b]);
        }
        for (std::uint64_t k = 0; k < kKeyframes; ++k) {
            for (std::uint64_t b = 0; b < bones_per_character; ++b) {
                const std::uint64_t t = (k * 6000 + phase[b]) % 360000;
                const std::uint64_t wave = t < 180000 ? t : 360000 - t;  // 0 .. 180000
                const auto angle = (static_cast<std::int64_t>(2 * wave) - 180000) * static_cast<std::int64_t>(amplitude[b]) / 180000;
                out.i32(angle);
            }
        }
    }
    set.bones = count * bones_per_character;
    set.keyframes = count > 0 ? kKeyframes : 0;
    set.sha256 = out.digest();
    return set;
}

// Local lights (the first `shadowed` cast shadows) and directional suns.
// One local light: position in millimetres, colour bytes, radius in millimetres, intensity in permille.
struct LightRecord {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
    std::array<std::uint64_t, 3> color{};
    std::uint64_t radius = 0;
    std::uint64_t intensity = 0;
    bool shadowed = false;
};

// One directional sun: azimuth and elevation in millidegrees.
struct SunRecord {
    std::uint64_t azimuth = 0;
    std::uint64_t elevation = 0;
};

struct LightSet {
    std::vector<LightRecord> lights;
    std::vector<SunRecord> suns;
};

LightSet place_lights(std::uint64_t seed, std::uint64_t lights, std::uint64_t shadowed, std::uint64_t suns, std::uint64_t world_seed) {
    LightSet set;
    Rng rng(seed, kSaltLight, 0);
    for (std::uint64_t l = 0; l < lights; ++l) {
        LightRecord light;
        light.x = static_cast<std::int64_t>(rng.below(kSizeM * kMm));
        light.z = static_cast<std::int64_t>(rng.below(kSizeM * kMm));
        light.y = terrain_height_mm(world_seed, light.x, light.z) + 2000 + static_cast<std::int64_t>(rng.below(8000));
        for (auto& channel : light.color) {
            channel = rng.below(256);
        }
        light.radius = 5000 + rng.below(20000);
        light.intensity = 500 + rng.below(3500);
        light.shadowed = l < shadowed;
        set.lights.push_back(light);
    }
    for (std::uint64_t s = 0; s < suns; ++s) {
        SunRecord sun;
        sun.azimuth = rng.below(360000);
        sun.elevation = 20000 + rng.below(50000);
        set.suns.push_back(sun);
    }
    return set;
}

std::string build_lights(const LightSet& set) {
    Bytes out;
    for (const LightRecord& light : set.lights) {
        out.i32(light.x);
        out.i32(light.y);
        out.i32(light.z);
        for (const std::uint64_t channel : light.color) {
            out.u8(channel);
        }
        out.u32(light.radius);
        out.u32(light.intensity);
        out.u8(light.shadowed ? 1 : 0);
    }
    for (const SunRecord& sun : set.suns) {
        out.u32(sun.azimuth);
        out.u32(sun.elevation);
    }
    return out.digest();
}

// Particle emitters, each steady at kParticlesPerEmitter live particles (rate times lifetime).
std::string build_particles(std::uint64_t seed, std::uint64_t emitters, std::uint64_t world_seed) {
    Bytes out;
    Rng rng(seed, kSaltParticle, 0);
    for (std::uint64_t e = 0; e < emitters; ++e) {
        const auto x = static_cast<std::int64_t>(rng.below(kSizeM * kMm));
        const auto z = static_cast<std::int64_t>(rng.below(kSizeM * kMm));
        const std::int64_t y = terrain_height_mm(world_seed, x, z) + static_cast<std::int64_t>(rng.below(3000));
        const std::uint64_t lifetime_ms = 500 + rng.below(1500);
        out.i32(x);
        out.i32(y);
        out.i32(z);
        out.u32(lifetime_ms);
        out.u32(1000000 / lifetime_ms);  // spawns per second
    }
    return out.digest();
}

// Dynamic bodies and static boxes around the centre, constraints between two different bodies, and physics characters.
PhysicsSpec build_physics(std::uint64_t seed, const Recipe& recipe, std::uint64_t world_seed) {
    PhysicsSpec spec;
    spec.centre_mm = kCentreMm;
    Rng rng(seed, kSaltPhysics, 0);
    for (std::uint64_t b = 0; b < recipe.bodies; ++b) {
        PhysicsBodySpec body;
        body.position_mm[0] = kCentreMm - 50000 + static_cast<std::int64_t>(rng.below(100000));
        body.position_mm[2] = kCentreMm - 50000 + static_cast<std::int64_t>(rng.below(100000));
        body.position_mm[1] = terrain_height_mm(world_seed, body.position_mm[0], body.position_mm[2]) + 5000 +
                              static_cast<std::int64_t>(rng.below(20000));
        for (int axis = 0; axis < 3; ++axis) {
            body.half_extent_mm[axis] = static_cast<std::uint32_t>(200 + rng.below(800));
        }
        body.mass_permille = static_cast<std::uint32_t>(500 + rng.below(4500));
        spec.bodies.push_back(body);
    }
    for (std::uint64_t s = 0; s < recipe.static_colliders; ++s) {
        PhysicsStaticSpec box;
        box.position_mm[0] = kCentreMm - 100000 + static_cast<std::int64_t>(rng.below(200000));
        box.position_mm[2] = kCentreMm - 100000 + static_cast<std::int64_t>(rng.below(200000));
        box.position_mm[1] = terrain_height_mm(world_seed, box.position_mm[0], box.position_mm[2]);
        for (int axis = 0; axis < 3; ++axis) {
            box.half_extent_mm[axis] = static_cast<std::uint32_t>(1000 + rng.below(4000));
        }
        spec.statics.push_back(box);
    }
    for (std::uint64_t c = 0; c < recipe.constraints; ++c) {
        PhysicsConstraintSpec constraint;
        const std::uint64_t a = rng.below(recipe.bodies);
        const std::uint64_t b = (a + 1 + rng.below(recipe.bodies - 1)) % recipe.bodies;
        constraint.body_a = static_cast<std::uint32_t>(a);
        constraint.body_b = static_cast<std::uint32_t>(b);
        spec.constraints.push_back(constraint);
    }
    for (std::uint64_t p = 0; p < recipe.physics_characters; ++p) {
        PhysicsCharacterSpec character;
        character.position_mm[0] = kCentreMm - 50000 + static_cast<std::int64_t>(rng.below(100000));
        character.position_mm[2] = kCentreMm - 50000 + static_cast<std::int64_t>(rng.below(100000));
        character.position_mm[1] = terrain_height_mm(world_seed, character.position_mm[0], character.position_mm[2]) + 1000;
        character.radius_mm = static_cast<std::uint32_t>(300 + rng.below(200));
        spec.characters.push_back(character);
    }
    return spec;
}

// The byte stream the `physics` digest covers, in the order the generator has always written it: bodies, statics, constraints, then
// characters. Changing this order changes the digest, so the expected file would need a new version.
std::string physics_digest(const PhysicsSpec& spec) {
    Bytes out;
    for (const PhysicsBodySpec& body : spec.bodies) {
        for (const std::int64_t value : body.position_mm) {
            out.i32(value);
        }
        for (const std::uint32_t half : body.half_extent_mm) {
            out.u32(half);
        }
        out.u32(body.mass_permille);
    }
    for (const PhysicsStaticSpec& box : spec.statics) {
        for (const std::int64_t value : box.position_mm) {
            out.i32(value);
        }
        for (const std::uint32_t half : box.half_extent_mm) {
            out.u32(half);
        }
    }
    for (const PhysicsConstraintSpec& constraint : spec.constraints) {
        out.u32(constraint.body_a);
        out.u32(constraint.body_b);
    }
    for (const PhysicsCharacterSpec& character : spec.characters) {
        for (const std::int64_t value : character.position_mm) {
            out.i32(value);
        }
        out.u32(character.radius_mm);
    }
    return out.digest();
}

// Audio emitters: a position, a tone frequency in Hz, and a gain in permille.
std::string build_audio(std::uint64_t seed, std::uint64_t emitters, std::uint64_t world_seed) {
    Bytes out;
    Rng rng(seed, kSaltAudio, 0);
    for (std::uint64_t e = 0; e < emitters; ++e) {
        const auto x = static_cast<std::int64_t>(rng.below(kSizeM * kMm));
        const auto z = static_cast<std::int64_t>(rng.below(kSizeM * kMm));
        out.i32(x);
        out.i32(terrain_height_mm(world_seed, x, z) + 1500);
        out.i32(z);
        out.u32(220 + rng.below(660));
        out.u32(200 + rng.below(800));
    }
    return out.digest();
}

struct TerrainWindow {
    std::uint64_t tiles = 0;
    std::uint64_t samples = 0;
    std::string sha256;
};

// The detailed residency window: window_cells x window_cells tiles centred on the world. Tiles sample the global 1 m grid, so
// a tile's edge samples are the next tile's first samples.
TerrainWindow build_terrain_window(const GeneratorWorld& world) {
    const std::int64_t first = world.grid_cells / 2 - world.window_cells / 2;
    const std::int64_t cell_mm = world.cell_m * kMm;
    Bytes out;
    out.reserve(static_cast<std::size_t>(world.window_cells * world.window_cells * world.tile_samples * world.tile_samples) * 4);
    for (std::int64_t cz = first; cz < first + world.window_cells; ++cz) {
        for (std::int64_t cx = first; cx < first + world.window_cells; ++cx) {
            for (std::int64_t sz = 0; sz < world.tile_samples; ++sz) {
                for (std::int64_t sx = 0; sx < world.tile_samples; ++sx) {
                    out.i32(terrain_height_mm(world.world_seed, cx * cell_mm + sx * kMm, cz * cell_mm + sz * kMm));
                }
            }
        }
    }
    TerrainWindow window;
    window.tiles = static_cast<std::uint64_t>(world.window_cells * world.window_cells);
    window.samples = window.tiles * static_cast<std::uint64_t>(world.tile_samples * world.tile_samples);
    window.sha256 = out.digest();
    return window;
}

// Built once per process: every scene shares the same world, so the window is the same for all of them.
const TerrainWindow& shared_terrain() {
    static const TerrainWindow window = build_terrain_window(generator_world());
    return window;
}

bool is_runtime_key(std::string_view key) {
    for (const auto runtime : kRuntimeKeys) {
        if (runtime == key) {
            return true;
        }
    }
    return false;
}

bool is_table_key(std::string_view key) {
    for (const auto& entry : kTableKeys) {
        if (entry.key == key) {
            return true;
        }
    }
    return false;
}

}  // namespace

GeneratorWorld generator_world() {
    GeneratorWorld world;
    world.size_m = kSizeM;
    world.cell_m = kCellM;
    world.grid_cells = kGridCells;
    world.window_cells = kWindowCells;
    world.tile_samples = kCellM + 1;
    world.world_seed = kWorldSeed;
    return world;
}

core::Result<core::Unit, std::string> check_generator_world(std::string_view world_json) {
    const Json world = Json::parse(world_json, nullptr, false);
    if (world.is_discarded() || !world.is_object()) {
        return core::failure(std::string("the world block is not a JSON object"));
    }
    const GeneratorWorld generated = generator_world();
    const std::pair<const char*, std::int64_t> fields[] = {
        {"size_m", generated.size_m},
        {"cell_m", generated.cell_m},
        {"grid_cells", generated.grid_cells},
        {"detail_window_cells", generated.window_cells},
        {"tile_height_samples", generated.tile_samples},
    };
    for (const auto& [key, value] : fields) {
        if (!world.contains(key) || !world.at(key).is_number_integer() || world.at(key).get<std::int64_t>() != value) {
            return core::failure(std::string("world.") + key + " must be " + std::to_string(value) + " to match the generator");
        }
    }
    return core::success();
}

std::int32_t terrain_height_mm(std::uint64_t world_seed, std::int64_t x_mm, std::int64_t z_mm) {
    const std::int64_t large = octave(world_seed, kSaltTerrainLarge, x_mm, z_mm, 128000, 30000);
    const std::int64_t small = octave(world_seed, kSaltTerrainSmall, x_mm, z_mm, 32000, 6000);
    return static_cast<std::int32_t>(kTerrainBaseMm + large + small);
}

// Reads the table counts of a scene into a recipe and refuses a combination the generator cannot build.
core::Result<Recipe, std::string> make_recipe(const std::string& name, const SceneDef& scene) {
    for (const auto& [key, value] : scene.counts) {
        (void)value;
        if (!is_table_key(key) && !is_runtime_key(key)) {
            return core::failure(name + ": workload key '" + key + "' is neither generated nor a runtime parameter");
        }
    }

    Recipe recipe;
    for (const auto& entry : kTableKeys) {
        const auto found = scene.counts.find(std::string(entry.key));
        if (found != scene.counts.end()) {
            recipe.*(entry.member) = found->second;
        }
    }
    if (recipe.instances > 0 && recipe.meshes == 0) {
        return core::failure(name + ": instances need at least one unique mesh");
    }
    if (recipe.characters > 0 && recipe.bones_per_character == 0) {
        return core::failure(name + ": animated characters need bones_per_character");
    }
    if (recipe.shadowed_lights > recipe.lights) {
        return core::failure(name + ": shadowed_local_lights exceeds local_lights");
    }
    if (recipe.particles % kParticlesPerEmitter != 0) {
        return core::failure(name + ": live particles must be a multiple of " + std::to_string(kParticlesPerEmitter));
    }
    if (recipe.constraints > 0 && recipe.bodies < 2) {
        return core::failure(name + ": constraints need at least two active bodies");
    }
    return core::success(recipe);
}

core::Result<Workload, std::string> generate_workload(std::string_view scene_name, const SceneDef& scene) {
    const std::string name(scene_name);
    if (scene.builtin) {
        return core::failure(name + ": a builtin scene is not a generated workload");
    }
    const auto made = make_recipe(name, scene);
    if (!made.ok()) {
        return core::failure(made.error());
    }
    const Recipe recipe = made.value();

    const GeneratorWorld world = generator_world();
    const MeshSet meshes = build_meshes(scene.seed, recipe.meshes);
    const InstanceSet instances = build_instances(place_instances(scene.seed, recipe.instances, recipe.meshes), meshes);
    const CharacterSet characters = build_characters(scene.seed, recipe.characters, recipe.bones_per_character);
    const LightSet lights = place_lights(scene.seed, recipe.lights, recipe.shadowed_lights, recipe.suns, world.world_seed);
    const PhysicsSpec physics = build_physics(scene.seed, recipe, world.world_seed);
    const TerrainWindow& terrain = shared_terrain();

    Workload workload;
    workload.scene = name;
    workload.seed = scene.seed;
    workload.world_seed = world.world_seed;
    workload.counts = {
        {"meshes", recipe.meshes},
        {"mesh_triangles", 0},
        {"instances", recipe.instances},
        {"instance_triangles", instances.triangles},
        {"instances_within_draw_distance", instances.within_draw_distance},
        {"characters", recipe.characters},
        {"bones_per_character", recipe.bones_per_character},
        {"bones", characters.bones},
        {"keyframes", characters.keyframes},
        {"lights", recipe.lights},
        {"shadowed_lights", recipe.shadowed_lights},
        {"suns", recipe.suns},
        {"particles", recipe.particles},
        {"particle_emitters", recipe.particles / kParticlesPerEmitter},
        {"bodies", recipe.bodies},
        {"static_colliders", recipe.static_colliders},
        {"constraints", recipe.constraints},
        {"physics_characters", recipe.physics_characters},
        {"audio_emitters", recipe.audio_emitters},
        {"terrain_tiles", terrain.tiles},
        {"terrain_samples", terrain.samples},
    };
    for (const std::uint64_t triangles : meshes.triangles) {
        workload.counts["mesh_triangles"] += triangles;
    }

    workload.sha256 = {
        {"meshes", meshes.sha256},
        {"instances", instances.sha256},
        {"characters", characters.sha256},
        {"lights", build_lights(lights)},
        {"particles", build_particles(scene.seed, recipe.particles / kParticlesPerEmitter, world.world_seed)},
        {"physics", physics_digest(physics)},
        {"audio", build_audio(scene.seed, recipe.audio_emitters, world.world_seed)},
        {"terrain", terrain.sha256},
    };
    return core::success(workload);
}

core::Result<PhysicsSpec, std::string> generate_physics_spec(std::string_view scene_name, const SceneDef& scene) {
    const std::string name(scene_name);
    if (scene.builtin) {
        return core::failure(name + ": a builtin scene is not a generated workload");
    }
    const auto made = make_recipe(name, scene);
    if (!made.ok()) {
        return core::failure(made.error());
    }
    return core::success(build_physics(scene.seed, made.value(), generator_world().world_seed));
}

core::Result<RenderContent, std::string> generate_render_content(const SceneDef& scene) {
    if (scene.builtin) {
        return core::failure(std::string("a builtin scene has no render content"));
    }
    const auto made = make_recipe("render content", scene);
    if (!made.ok()) {
        return core::failure(made.error());
    }
    const Recipe recipe = made.value();
    const GeneratorWorld world = generator_world();

    RenderContent content;
    for (std::uint64_t i = 0; i < recipe.meshes; ++i) {
        const MeshGeometry geometry = build_mesh_geometry(scene.seed, i);
        RenderMesh mesh;
        for (std::size_t v = 0; v < geometry.vertices_mm.size(); v += 3) {
            mesh.positions.push_back({static_cast<float>(geometry.vertices_mm[v]) / kMm,
                                      static_cast<float>(geometry.vertices_mm[v + 1]) / kMm,
                                      static_cast<float>(geometry.vertices_mm[v + 2]) / kMm});
        }
        mesh.indices = geometry.indices;
        content.meshes.push_back(std::move(mesh));
    }
    for (const InstanceRecord& record : place_instances(scene.seed, recipe.instances, recipe.meshes)) {
        RenderInstance instance;
        instance.position = {static_cast<double>(record.x) / kMm, static_cast<double>(record.y) / kMm,
                             static_cast<double>(record.z) / kMm};
        instance.mesh = static_cast<std::uint32_t>(record.mesh);
        instance.yaw_deg = static_cast<double>(record.yaw) / 1000.0;
        instance.scale = static_cast<double>(record.scale) / 1000.0;
        content.instances.push_back(instance);
    }
    const LightSet lights = place_lights(scene.seed, recipe.lights, recipe.shadowed_lights, recipe.suns, world.world_seed);
    for (std::size_t l = 0; l < lights.lights.size(); ++l) {
        const LightRecord& record = lights.lights[l];
        RenderLight light;
        light.id = static_cast<std::uint32_t>(l + 1);
        light.position = {static_cast<double>(record.x) / kMm, static_cast<double>(record.y) / kMm, static_cast<double>(record.z) / kMm};
        light.color = {static_cast<float>(record.color[0]) / 255.0f, static_cast<float>(record.color[1]) / 255.0f,
                       static_cast<float>(record.color[2]) / 255.0f};
        light.radius_m = static_cast<double>(record.radius) / kMm;
        light.intensity = static_cast<double>(record.intensity) / 1000.0;
        light.shadowed = record.shadowed;
        content.lights.push_back(light);
    }
    for (const SunRecord& sun : lights.suns) {
        content.suns.push_back({static_cast<double>(sun.azimuth) / 1000.0, static_cast<double>(sun.elevation) / 1000.0});
    }
    return core::success(content);
}

std::vector<std::string> compare_workloads(const Workload& expected, const Workload& actual) {
    std::vector<std::string> diffs;
    if (expected.seed != actual.seed) {
        diffs.push_back("seed: expected " + std::to_string(expected.seed) + ", got " + std::to_string(actual.seed));
    }
    if (expected.world_seed != actual.world_seed) {
        diffs.push_back("world_seed: expected " + std::to_string(expected.world_seed) + ", got " + std::to_string(actual.world_seed));
    }
    std::set<std::string> count_keys;
    for (const auto& [key, value] : expected.counts) {
        (void)value;
        count_keys.insert(key);
    }
    for (const auto& [key, value] : actual.counts) {
        (void)value;
        count_keys.insert(key);
    }
    for (const auto& key : count_keys) {
        const auto e = expected.counts.find(key);
        const auto a = actual.counts.find(key);
        if (e == expected.counts.end() || a == actual.counts.end()) {
            diffs.push_back("count " + key + ": missing on one side");
        } else if (e->second != a->second) {
            diffs.push_back("count " + key + ": expected " + std::to_string(e->second) + ", got " + std::to_string(a->second));
        }
    }
    std::set<std::string> digest_keys;
    for (const auto& [key, value] : expected.sha256) {
        (void)value;
        digest_keys.insert(key);
    }
    for (const auto& [key, value] : actual.sha256) {
        (void)value;
        digest_keys.insert(key);
    }
    for (const auto& key : digest_keys) {
        const auto e = expected.sha256.find(key);
        const auto a = actual.sha256.find(key);
        if (e == expected.sha256.end() || a == actual.sha256.end()) {
            diffs.push_back("sha256 " + key + ": missing on one side");
        } else if (e->second != a->second) {
            diffs.push_back("sha256 " + key + ": digest differs");
        }
    }
    return diffs;
}

std::string workloads_json(const std::map<std::string, Workload>& scenes) {
    Json entries = Json::object();
    for (const auto& [name, workload] : scenes) {
        Json entry;
        entry["seed"] = workload.seed;
        entry["world_seed"] = workload.world_seed;
        entry["counts"] = workload.counts;
        entry["sha256"] = workload.sha256;
        entries[name] = entry;
    }
    Json root;
    root["workload_version"] = kWorkloadVersion;
    root["scenes"] = entries;
    return root.dump(2) + "\n";
}

core::Result<std::map<std::string, Workload>, std::string> parse_workloads(std::string_view text) {
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return core::failure(std::string("the expected file is not a JSON object"));
    }
    if (!root.contains("workload_version") || !root.at("workload_version").is_number_unsigned() ||
        root.at("workload_version").get<std::uint64_t>() != static_cast<std::uint64_t>(kWorkloadVersion)) {
        return core::failure("workload_version must be " + std::to_string(kWorkloadVersion));
    }
    if (!root.contains("scenes") || !root.at("scenes").is_object()) {
        return core::failure(std::string("'scenes' must be an object"));
    }
    std::map<std::string, Workload> scenes;
    for (const auto& [name, entry] : root.at("scenes").items()) {
        const std::string where = "scene " + name;
        if (!entry.is_object() || !entry.contains("seed") || !entry.at("seed").is_number_unsigned() ||
            !entry.contains("world_seed") || !entry.at("world_seed").is_number_unsigned() || !entry.contains("counts") ||
            !entry.at("counts").is_object() || !entry.contains("sha256") || !entry.at("sha256").is_object()) {
            return core::failure(where + ": needs seed, world_seed, counts, and sha256");
        }
        Workload workload;
        workload.scene = name;
        workload.seed = entry.at("seed").get<std::uint64_t>();
        workload.world_seed = entry.at("world_seed").get<std::uint64_t>();
        for (const auto& [key, value] : entry.at("counts").items()) {
            if (!value.is_number_unsigned()) {
                return core::failure(where + ": count '" + key + "' must be an unsigned integer");
            }
            workload.counts[key] = value.get<std::uint64_t>();
        }
        for (const auto& [key, value] : entry.at("sha256").items()) {
            if (!value.is_string()) {
                return core::failure(where + ": digest '" + key + "' must be a string");
            }
            workload.sha256[key] = value.get<std::string>();
        }
        scenes[name] = workload;
    }
    return core::success(scenes);
}

}  // namespace engine::bench
