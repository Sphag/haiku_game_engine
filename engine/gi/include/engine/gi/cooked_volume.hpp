#pragma once

// The runtime side of the gi_probes payload (US-040). The converter writes "<output>/<guid>.cooked" for each gi_probes asset: the cooked
// container (engine/assets/cooked_container.hpp) around a serialized GiVolume. The runtime finds the asset by its source path in the
// registry, reads that file, and decodes the volume without baking. Nothing here bakes, so loading does not need the converter.

#include "engine/assets/asset_registry.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"
#include "engine/gi/probes.hpp"

#include <string>
#include <string_view>

namespace engine::gi {

// Decodes cooked container bytes into a volume. The container and the volume each check their own magic and version, so a stale or
// foreign file is rejected with a reason.
[[nodiscard]] core::Result<GiVolume, std::string> decode_cooked_volume(std::string_view cooked_bytes);

// Loads the volume for the gi_probes asset at `source_path`. The registry must hold that asset with the "gi_probes" importer, and its
// cooked file must exist in `output_dir`. Any other case is an error, and the caller falls back to the environment.
[[nodiscard]] core::Result<GiVolume, std::string> load_gi_volume(const assets::AssetRegistry& registry, const core::Path& output_dir,
                                                                 const core::Path& source_path);

}  // namespace engine::gi
