#pragma once

// Project scan (US-018): builds an AssetRegistry from the sidecar metadata files under a project directory.
//
// Each source "<name>" has a sidecar "<name>.meta" beside it. The sidecar is the only place the source's GUID is stored, so
// a source moved or renamed together with its sidecar keeps its GUID. A copy of a source with its sidecar claims the same
// GUID twice, and the scan rejects it with GuidConflict.
//
//   {
//     "format": "engine.asset_meta",
//     "version": 1,
//     "guid": "6b1f2a4e-0c3d-4e5f-8a9b-1c2d3e4f5a60",
//     "importer": { "type": "texture", "settings": { "srgb": true } },
//     "dependencies": [ "7c2e3b5f-1d4e-4f60-9b0c-2d3e4f5a6b71" ]
//   }
//
// The importer that reads a source declares its dependencies in the sidecar. The registry stores and checks them; it does not
// derive them from the source bytes. Unknown keys, a different format name, and an unsupported version are errors.
//
// A source without a sidecar is reported as unregistered and is not registered. A sidecar whose source is missing is reported
// as orphaned. Neither is an error, because a converter creates the first and a user deletes the second.
//
// A scan builds a new registry and returns it only when every sidecar is valid. Any error leaves nothing behind.

#include "engine/assets/asset_registry.hpp"

#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

inline constexpr std::string_view kAssetMetaFormat = "engine.asset_meta";
inline constexpr std::string_view kAssetMetaSuffix = ".meta";
inline constexpr int kAssetMetaVersion = 1;

enum class ScanErrorCode {
    Io,                 // a directory, file, or sidecar could not be read or written
    Malformed,          // not JSON, an unknown key, a wrong member type, a nil or invalid GUID, or an invalid path or importer
    WrongFormat,        // the format name is not engine.asset_meta
    UnsupportedVersion, // the version is not kAssetMetaVersion
    GuidConflict,       // two sources claim the same GUID
    PathConflict,       // two sources claim the same path
    MissingSource,      // create_asset_metadata: the source file does not exist
    AlreadyExists,      // create_asset_metadata: a sidecar already exists for the source
};

struct ScanError {
    ScanErrorCode code;
    std::string path;     // the project-relative file the error concerns; empty when none applies
    std::string message;  // what went wrong, for the log
};

struct ProjectScan {
    AssetRegistry registry;
    std::vector<std::string> unregistered;     // source paths with no sidecar, sorted
    std::vector<std::string> orphan_metadata;  // sidecar paths whose source is missing, sorted
};

// Scans the directory tree under root. Symbolic links are not followed. Every file that does not end in ".meta" is a source.
[[nodiscard]] core::Result<ProjectScan, ScanError> scan_project(const core::Path& root);

// Writes "<source>.meta" with a new GUID and no dependencies. `source` is a project-relative path. The source must exist and
// must not already have a sidecar. The settings text must be a JSON object; it is stored in canonical form. Returns the new GUID.
[[nodiscard]] core::Result<core::Guid, ScanError> create_asset_metadata(const core::Path& root, const core::Path& source,
                                                                       const std::string& importer_type,
                                                                       std::string_view settings_json);

// Rewrites "<source>.meta" with new importer settings. The GUID, the importer type, and the dependencies are kept. The settings
// text must be a JSON object and is stored in canonical form. The caller checks the settings against the importer first.
[[nodiscard]] core::Result<core::Unit, ScanError> update_asset_metadata(const core::Path& root, const core::Path& source,
                                                                       std::string_view settings_json);

// Moves a registered source and its sidecar to a new project-relative path, so its GUID stays the same. The target must not exist.
// Fails without moving anything when the target is taken; a failed sidecar move puts the source back.
[[nodiscard]] core::Result<core::Unit, ScanError> move_asset(const core::Path& root, const core::Path& source, const core::Path& target);

// Deletes a source and its sidecar. The source goes first, so a failed delete leaves the sidecar and the asset registered.
[[nodiscard]] core::Result<core::Unit, ScanError> remove_asset(const core::Path& root, const core::Path& source);

}  // namespace engine::assets
