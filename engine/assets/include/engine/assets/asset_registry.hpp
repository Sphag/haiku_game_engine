#pragma once

// The asset registry (US-018): one record per asset, keyed by persistent GUID, with its source path, importer, importer
// settings, content hash, and the GUIDs it depends on.
//
// References are GUIDs, never paths. Renaming or moving a source changes only its path, so scenes and other assets that
// name the GUID keep working. A dependency may name an asset that is not registered: the edge stays, and
// missing_dependencies() reports it. Deleting an asset leaves its dependents' edges in place for the same reason.
//
// The registry is an in-memory index. The sidecar metadata files in the project are the persistent source of truth; see
// asset_project.hpp, which builds a registry from them.
//
// Not thread-safe: the owning thread changes the registry at a synchronization point.

#include "engine/assets/content_hash.hpp"

#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace engine::assets {

enum class RegistryErrorCode {
    NilGuid,         // the asset's GUID is nil
    GuidConflict,    // another registered asset already has this GUID
    PathConflict,    // another registered asset already uses this source path
    NotFound,        // no registered asset has this GUID
    InvalidPath,     // the source path is empty or absolute; paths are relative to the project root
    EmptyImporter,   // the importer type name is empty
    SelfDependency,  // the asset depends on itself
};

struct RegistryError {
    RegistryErrorCode code;
    std::string message;
};

struct AssetRecord {
    core::Guid guid;
    core::Path source_path;               // relative to the project root, normalized
    std::string importer;                 // importer type, for example "texture"
    std::string importer_settings;        // canonical JSON text of the importer's settings
    ContentHash content_hash;             // digest of the source bytes when the registry was last filled
    std::vector<core::Guid> dependencies;  // sorted and unique after add(); may name unregistered assets
};

// An edge whose target is not registered. `asset` is the dependent.
struct MissingDependency {
    core::Guid asset;
    core::Guid dependency;
};

class AssetRegistry {
public:
    // Adds the record after checking it. On failure the registry is unchanged.
    [[nodiscard]] core::Result<core::Unit, RegistryError> add(AssetRecord record);

    // Removes the asset. Returns the registered assets that depend on it, in GUID order. Their edges remain and now report
    // the removed GUID as missing.
    [[nodiscard]] core::Result<std::vector<core::Guid>, RegistryError> remove(const core::Guid& guid);

    // Changes the source path only. The GUID and every edge stay as they are. Fails with PathConflict when another asset
    // uses the new path; on failure the registry is unchanged.
    [[nodiscard]] core::Result<core::Unit, RegistryError> move(const core::Guid& guid, const core::Path& new_path);

    [[nodiscard]] const AssetRecord* find(const core::Guid& guid) const;
    [[nodiscard]] const AssetRecord* find_by_path(const core::Path& source_path) const;

    // Every record, in GUID order.
    [[nodiscard]] std::vector<const AssetRecord*> records() const;

    [[nodiscard]] std::size_t size() const noexcept {
        return records_.size();
    }

    // The registered assets whose dependencies include the GUID, in GUID order. Works for an unregistered GUID too.
    [[nodiscard]] std::vector<core::Guid> dependents(const core::Guid& guid) const;

    // Every edge whose target is not registered, ordered by dependent and then by target.
    [[nodiscard]] std::vector<MissingDependency> missing_dependencies() const;

    // Every GUID reachable from the asset through dependency edges, sorted, without the asset itself. Unregistered targets
    // are included and are not expanded. Safe on cyclic graphs. Empty when the asset is not registered.
    [[nodiscard]] std::vector<core::Guid> dependency_closure(const core::Guid& guid) const;

private:
    std::map<core::Guid, AssetRecord> records_;
    std::map<std::string, core::Guid> by_path_;  // source path text to GUID
};

}  // namespace engine::assets
