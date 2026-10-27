#pragma once

#include "engine/rhi/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::rhi {

// Optional GPU features. Mesh shaders, indirect count, and ray tracing are separate features: an adapter can have one
// without the others, and each one gates its own commands and pipelines.
enum class Feature : std::uint32_t {
    None = 0,
    MeshShaders = 1u << 0,
    IndirectCount = 1u << 1,
    RayTracing = 1u << 2,
    TimestampQueries = 1u << 3,
};
template <>
struct EnableFlagOperators<Feature> : std::true_type {};

inline constexpr std::size_t kFeatureCount = 4;
inline constexpr Feature kAllFeatures = Feature::MeshShaders | Feature::IndirectCount | Feature::RayTracing | Feature::TimestampQueries;
inline constexpr std::array<Feature, kFeatureCount> kFeatureList{
    Feature::MeshShaders,
    Feature::IndirectCount,
    Feature::RayTracing,
    Feature::TimestampQueries,
};

// Position of a single feature in CapabilityReport::features.
[[nodiscard]] constexpr std::size_t feature_index(Feature feature) noexcept {
    switch (feature) {
        case Feature::MeshShaders:
            return 0;
        case Feature::IndirectCount:
            return 1;
        case Feature::RayTracing:
            return 2;
        case Feature::TimestampQueries:
            return 3;
        case Feature::None:
            break;
    }
    return 0;
}

// Unsupported: the adapter does not have the feature. DisabledForTest: the adapter has it, but the device was created with
// the feature forced off. Only Supported is enabled, so a forced-off feature is never reported as available.
enum class FeatureState : std::uint8_t {
    Unsupported,
    DisabledForTest,
    Supported,
};
[[nodiscard]] std::string_view to_string(FeatureState state) noexcept;

struct CapabilityReport {
    Backend backend = Backend::Validation;
    std::string adapter_name;
    std::array<FeatureState, kFeatureCount> features{};  // zero-initialized: every feature Unsupported
    std::uint64_t timestamp_frequency_hz = 0;             // ticks per second for read_timestamp
    bool validation_layers = false;                       // the backend's debug/validation layers are active

    [[nodiscard]] FeatureState state(Feature feature) const noexcept {
        return features[feature_index(feature)];
    }
    [[nodiscard]] bool enabled(Feature feature) const noexcept {
        return state(feature) == FeatureState::Supported;
    }
};

}  // namespace engine::rhi
