#include "engine/gi/cooked_volume.hpp"

#include "engine/assets/cooked_container.hpp"

#include <string>
#include <string_view>

namespace engine::gi {

using core::Result;
using core::failure;

Result<GiVolume, std::string> decode_cooked_volume(std::string_view cooked_bytes) {
    const auto payload = assets::read_cooked_container(cooked_bytes);
    if (!payload.ok()) {
        return core::failure("cooked GI volume: " + payload.error());
    }
    return deserialize(payload.value());
}

Result<GiVolume, std::string> load_gi_volume(const assets::AssetRegistry& registry, const core::Path& output_dir,
                                             const core::Path& source_path) {
    const assets::AssetRecord* record = registry.find_by_path(source_path);
    if (record == nullptr) {
        return core::failure("no asset is registered for " + source_path.str());
    }
    if (record->importer != "gi_probes") {
        return core::failure(source_path.str() + " is a '" + record->importer + "' asset, not gi_probes");
    }
    const auto cooked_path = output_dir.join(record->guid.to_string() + ".cooked");
    if (!cooked_path.ok()) {
        return core::failure("the output directory cannot hold " + record->guid.to_string() + ".cooked: " +
                             std::string(core::to_string(cooked_path.error())));
    }
    const auto bytes = assets::read_cooked_file(cooked_path.value());
    if (!bytes.ok()) {
        return core::failure("cannot read the cooked GI volume for " + source_path.str() + ": " + bytes.error());
    }
    return decode_cooked_volume(bytes.value());
}

}  // namespace engine::gi
