// The gi_probes importer (US-040). Its source is a JSON scene description: the probe grid, the analytic sky, and the axis-aligned
// occluders. The payload is the baked probe volume (engine/gi/probes.hpp), so the runtime reads the result without baking.
//
// Source document (every key except grid and occluders is optional):
//   {
//     "grid": {"origin": [x, y, z], "spacing": s, "dims": [nx, ny, nz]},
//     "sky": {"zenith": [r, g, b], "horizon": [r, g, b], "ground": [r, g, b],
//             "sun_direction": [x, y, z], "sun_radiance": [r, g, b]},
//     "occluders": [{"min": [x, y, z], "max": [x, y, z], "albedo": [r, g, b], "emitted": [r, g, b]}],
//     "samples": 256,
//     "max_ray_distance": 1000
//   }
// The settings take no keys, so the cache key of a bake is its source bytes plus this importer's version.

#include "asset_converter/importer.hpp"

#include "engine/core/result.hpp"
#include "engine/gi/probes.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace engine::converter {
namespace {

using Json = nlohmann::json;
using core::Result;
using core::failure;
using core::success;

// Version 1: the bake math of engine/gi with the format in kFormatVersion. Bump it when the output changes for the same source.
constexpr int kGiImporterVersion = 1;

glm::vec3 read_vec3(const Json& object, const char* key, const glm::vec3& fallback) {
    if (!object.contains(key)) {
        return fallback;
    }
    const Json& value = object.at(key);
    if (!value.is_array() || value.size() != 3) {
        throw std::invalid_argument(std::string("'") + key + "' must be an array of three numbers");
    }
    return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
}

// A whole, non-negative number that fits in the counts the bake uses. Anything else is rejected before it reaches the bake.
std::uint32_t read_count(const Json& value) {
    if (!value.is_number()) {
        throw std::invalid_argument("'dims' must be whole, non-negative counts");
    }
    const double count = value.get<double>();
    if (!(count >= 0.0) || count != std::floor(count) || count > static_cast<double>(engine::gi::kMaxProbes)) {
        throw std::invalid_argument("'dims' must be whole, non-negative counts");
    }
    return static_cast<std::uint32_t>(count);
}

// Builds the bake inputs from the source document. Anything missing or mistyped is reported with the key it came from.
engine::gi::BakeInputs parse_source(const Json& document) {
    if (!document.is_object() || !document.contains("grid") || !document.contains("occluders")) {
        throw std::invalid_argument("the source needs a 'grid' object and an 'occluders' array");
    }
    engine::gi::BakeInputs inputs;

    const Json& grid = document.at("grid");
    inputs.grid.origin = read_vec3(grid, "origin", glm::vec3(0.0f));
    inputs.grid.spacing = grid.at("spacing").get<float>();
    const Json& dims = grid.at("dims");
    if (!dims.is_array() || dims.size() != 3) {
        throw std::invalid_argument("'dims' must be an array of three whole, non-negative counts");
    }
    inputs.grid.dims = glm::uvec3(read_count(dims[0]), read_count(dims[1]), read_count(dims[2]));

    if (document.contains("sky")) {
        const Json& sky = document.at("sky");
        const engine::gi::Sky defaults;
        inputs.sky.zenith = read_vec3(sky, "zenith", defaults.zenith);
        inputs.sky.horizon = read_vec3(sky, "horizon", defaults.horizon);
        inputs.sky.ground = read_vec3(sky, "ground", defaults.ground);
        inputs.sky.sun_direction = read_vec3(sky, "sun_direction", defaults.sun_direction);
        inputs.sky.sun_radiance = read_vec3(sky, "sun_radiance", defaults.sun_radiance);
    }

    for (const Json& occluder : document.at("occluders")) {
        engine::gi::Box box;
        box.min = read_vec3(occluder, "min", glm::vec3(0.0f));
        box.max = read_vec3(occluder, "max", glm::vec3(0.0f));
        box.albedo = read_vec3(occluder, "albedo", glm::vec3(0.5f));
        box.emitted = read_vec3(occluder, "emitted", glm::vec3(0.0f));
        inputs.boxes.push_back(box);
    }

    if (document.contains("samples")) {
        inputs.samples = document.at("samples").get<std::uint32_t>();
    }
    if (document.contains("max_ray_distance")) {
        inputs.max_ray_distance = document.at("max_ray_distance").get<float>();
    }
    return inputs;
}

class GiProbesImporter final : public Importer {
public:
    [[nodiscard]] std::string_view type() const noexcept override {
        return "gi_probes";
    }
    [[nodiscard]] int version() const noexcept override {
        return kGiImporterVersion;
    }

    [[nodiscard]] Result<std::string, std::string> canonical_settings(std::string_view settings_json) const override {
        Json settings = Json::parse(std::string(settings_json), nullptr, false);
        if (settings.is_discarded() || !settings.is_object()) {
            return failure(std::string("settings must be a JSON object"));
        }
        if (!settings.empty()) {
            return failure(std::string("this importer takes no settings"));
        }
        return success(std::string("{}"));
    }

    [[nodiscard]] Result<ImportOutput, std::string> import(std::string_view source, std::string_view) const override {
        engine::gi::BakeInputs inputs;
        try {
            const Json document = Json::parse(std::string(source));
            inputs = parse_source(document);
        } catch (const std::exception& error) {
            return failure("gi_probes source: " + std::string(error.what()));
        }
        if (auto problem = engine::gi::validate_inputs(inputs)) {
            return failure("gi_probes source: " + *problem);
        }
        ImportOutput output;
        output.payload = engine::gi::serialize(engine::gi::bake_volume(inputs));
        return success(std::move(output));
    }
};

}  // namespace

std::unique_ptr<Importer> make_gi_probes_importer() {
    return std::make_unique<GiProbesImporter>();
}

}  // namespace engine::converter
