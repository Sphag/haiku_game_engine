#include "engine/assets/asset_project.hpp"

#include "engine/assets/content_hash.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace engine::assets {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;
using core::Guid;
using core::Path;
using core::Result;
using core::Unit;
using core::failure;
using core::success;

ScanError scan_error(ScanErrorCode code, std::string path, std::string message) {
    return ScanError{code, std::move(path), std::move(message)};
}

ScanError malformed(const std::string& path, std::string message) {
    return scan_error(ScanErrorCode::Malformed, path, std::move(message));
}

ScanError io_failure(const std::string& path, std::string message) {
    return scan_error(ScanErrorCode::Io, path, std::move(message));
}

// The UTF-8 text of a path, with forward slashes.
std::string utf8(const fs::path& value) {
    const auto text = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

bool has_suffix(const std::string& text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The member of an object, or nullptr when it is absent.
const Json* member(const Json& object, const char* name) {
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}

// Rejects a key the format does not define. Unknown keys are errors, so a newer file is never read partly.
std::optional<ScanError> check_keys(const Json& object, std::initializer_list<const char*> allowed, const std::string& path) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const char* name : allowed) {
            known = known || it.key() == name;
        }
        if (!known) {
            return malformed(path, "unknown key '" + it.key() + "'");
        }
    }
    return std::nullopt;
}

// The file at a project-relative path, resolved against the root.
Result<Path, ScanError> project_path(const Path& root, const std::string& relative) {
    auto joined = Path::from_utf8(root.str() + "/" + relative);
    if (!joined.ok()) {
        return failure(malformed(relative, "path is not valid UTF-8 or contains a NUL character"));
    }
    return success(std::move(joined).value());
}

// Parses one sidecar. `source` is the project-relative path of the source it describes; the record's content hash is left
// empty for the caller to fill from the source bytes.
Result<AssetRecord, ScanError> parse_sidecar(const std::string& text, const std::string& source, const std::string& sidecar) {
    const Json doc = Json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return failure(malformed(sidecar, "not a JSON object"));
    }
    if (auto problem = check_keys(doc, {"format", "version", "guid", "importer", "dependencies"}, sidecar)) {
        return failure(*problem);
    }

    const Json* format = member(doc, "format");
    if (format == nullptr || !format->is_string() || format->get<std::string>() != kAssetMetaFormat) {
        return failure(scan_error(ScanErrorCode::WrongFormat, sidecar, "expected format '" + std::string(kAssetMetaFormat) + "'"));
    }
    const Json* version = member(doc, "version");
    if (version == nullptr || !version->is_number_integer() || version->get<std::int64_t>() != kAssetMetaVersion) {
        return failure(scan_error(ScanErrorCode::UnsupportedVersion, sidecar,
                                  "expected version " + std::to_string(kAssetMetaVersion)));
    }

    const Json* guid_member = member(doc, "guid");
    if (guid_member == nullptr || !guid_member->is_string()) {
        return failure(malformed(sidecar, "'guid' must be a string"));
    }
    auto guid = Guid::parse(guid_member->get<std::string>());
    if (!guid.ok()) {
        return failure(malformed(sidecar, "'guid' is not a GUID"));
    }

    const Json* importer = member(doc, "importer");
    if (importer == nullptr || !importer->is_object()) {
        return failure(malformed(sidecar, "'importer' must be an object"));
    }
    if (auto problem = check_keys(*importer, {"type", "settings"}, sidecar)) {
        return failure(*problem);
    }
    const Json* type = member(*importer, "type");
    if (type == nullptr || !type->is_string() || type->get<std::string>().empty()) {
        return failure(malformed(sidecar, "'importer.type' must be a non-empty string"));
    }
    std::string settings = "{}";
    if (const Json* settings_member = member(*importer, "settings")) {
        if (!settings_member->is_object()) {
            return failure(malformed(sidecar, "'importer.settings' must be an object"));
        }
        settings = settings_member->dump();
    }

    std::vector<Guid> dependencies;
    if (const Json* list = member(doc, "dependencies")) {
        if (!list->is_array()) {
            return failure(malformed(sidecar, "'dependencies' must be an array"));
        }
        for (const Json& item : *list) {
            if (!item.is_string()) {
                return failure(malformed(sidecar, "a dependency is not a GUID string"));
            }
            auto dependency = Guid::parse(item.get<std::string>());
            if (!dependency.ok()) {
                return failure(malformed(sidecar, "a dependency is not a GUID string"));
            }
            dependencies.push_back(dependency.value());
        }
    }

    auto source_path = Path::from_utf8(source);
    if (!source_path.ok()) {
        return failure(malformed(sidecar, "source path is not valid UTF-8"));
    }
    return success(AssetRecord{guid.value(), std::move(source_path).value(), type->get<std::string>(), std::move(settings),
                               ContentHash{}, std::move(dependencies)});
}

// Maps a registry rejection onto the scan's error codes. The message already names the GUID or path.
ScanError registry_failure(const RegistryError& error, const std::string& source) {
    switch (error.code) {
        case RegistryErrorCode::GuidConflict:
            return scan_error(ScanErrorCode::GuidConflict, source, error.message);
        case RegistryErrorCode::PathConflict:
            return scan_error(ScanErrorCode::PathConflict, source, error.message);
        default:
            return malformed(source, error.message);
    }
}

}  // namespace

Result<ProjectScan, ScanError> scan_project(const Path& root) {
    const fs::path native_root = core::to_native_path(root);
    std::error_code ec;
    if (!fs::is_directory(native_root, ec) || ec) {
        return failure(io_failure(root.str(), "project root is not a directory"));
    }

    // Walk the tree once. Sidecars and sources are kept apart, and both are sorted so every result is deterministic.
    std::set<std::string> sources;
    std::set<std::string> sidecars;
    fs::recursive_directory_iterator walker(native_root, ec);
    if (ec) {
        return failure(io_failure(root.str(), "cannot list the project: " + ec.message()));
    }
    for (; walker != fs::recursive_directory_iterator(); walker.increment(ec)) {
        if (ec) {
            return failure(io_failure(root.str(), "cannot list the project: " + ec.message()));
        }
        if (!walker->is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        const std::string relative = utf8(walker->path().lexically_relative(native_root));
        if (has_suffix(relative, kAssetMetaSuffix)) {
            sidecars.insert(relative);
        } else {
            sources.insert(relative);
        }
    }

    ProjectScan scan;
    for (const std::string& sidecar : sidecars) {
        const std::string source = sidecar.substr(0, sidecar.size() - kAssetMetaSuffix.size());
        if (!sources.contains(source)) {
            scan.orphan_metadata.push_back(sidecar);
            continue;
        }

        auto sidecar_file = project_path(root, sidecar);
        if (!sidecar_file.ok()) {
            return failure(sidecar_file.error());
        }
        auto text = core::read_text_file(sidecar_file.value());
        if (!text.ok()) {
            return failure(io_failure(sidecar, core::describe(text.error())));
        }
        auto parsed = parse_sidecar(text.value(), source, sidecar);
        if (!parsed.ok()) {
            return failure(parsed.error());
        }
        AssetRecord record = std::move(parsed).value();

        auto source_file = project_path(root, source);
        if (!source_file.ok()) {
            return failure(source_file.error());
        }
        auto bytes = core::read_file(source_file.value());
        if (!bytes.ok()) {
            return failure(io_failure(source, core::describe(bytes.error())));
        }
        record.content_hash = ContentHash::of(bytes.value());

        if (const auto added = scan.registry.add(std::move(record)); !added.ok()) {
            return failure(registry_failure(added.error(), source));
        }
    }

    for (const std::string& source : sources) {
        if (!sidecars.contains(source + std::string(kAssetMetaSuffix))) {
            scan.unregistered.push_back(source);
        }
    }
    return success(std::move(scan));
}

Result<Guid, ScanError> create_asset_metadata(const Path& root, const Path& source, const std::string& importer_type,
                                              std::string_view settings_json) {
    const std::string source_text = source.str();
    auto source_file = project_path(root, source_text);
    if (!source_file.ok()) {
        return failure(source_file.error());
    }
    if (importer_type.empty()) {
        return failure(malformed(source_text, "importer type is empty"));
    }
    if (!core::path_exists(source_file.value())) {
        return failure(scan_error(ScanErrorCode::MissingSource, source_text, "the source does not exist"));
    }
    auto sidecar_file = project_path(root, source_text + std::string(kAssetMetaSuffix));
    if (!sidecar_file.ok()) {
        return failure(sidecar_file.error());
    }
    if (core::path_exists(sidecar_file.value())) {
        return failure(scan_error(ScanErrorCode::AlreadyExists, source_text, "the source already has a sidecar"));
    }

    const Json settings = Json::parse(std::string(settings_json), nullptr, false);
    if (settings.is_discarded() || !settings.is_object()) {
        return failure(malformed(source_text, "importer settings must be a JSON object"));
    }

    const Guid guid = Guid::generate();
    Json doc = Json::object();
    doc["format"] = std::string(kAssetMetaFormat);
    doc["version"] = kAssetMetaVersion;
    doc["guid"] = guid.to_string();
    doc["importer"] = Json::object();
    doc["importer"]["type"] = importer_type;
    doc["importer"]["settings"] = settings;
    doc["dependencies"] = Json::array();

    if (auto written = core::write_file_atomic(sidecar_file.value(), doc.dump(2) + "\n"); !written.ok()) {
        return failure(io_failure(source_text, core::describe(written.error())));
    }
    return success(guid);
}

Result<Unit, ScanError> update_asset_metadata(const Path& root, const Path& source, std::string_view settings_json) {
    const std::string source_text = source.str();
    auto sidecar_file = project_path(root, source_text + std::string(kAssetMetaSuffix));
    if (!sidecar_file.ok()) {
        return failure(sidecar_file.error());
    }
    auto text = core::read_text_file(sidecar_file.value());
    if (!text.ok()) {
        return failure(io_failure(source_text, core::describe(text.error())));
    }
    auto record = parse_sidecar(text.value(), source_text, source_text + std::string(kAssetMetaSuffix));
    if (!record.ok()) {
        return failure(record.error());
    }

    const Json settings = Json::parse(std::string(settings_json), nullptr, false);
    if (settings.is_discarded() || !settings.is_object()) {
        return failure(malformed(source_text, "importer settings must be a JSON object"));
    }

    const AssetRecord& asset = record.value();
    Json doc = Json::object();
    doc["format"] = std::string(kAssetMetaFormat);
    doc["version"] = kAssetMetaVersion;
    doc["guid"] = asset.guid.to_string();
    doc["importer"] = Json::object();
    doc["importer"]["type"] = asset.importer;
    doc["importer"]["settings"] = settings;
    doc["dependencies"] = Json::array();
    for (const Guid& dependency : asset.dependencies) {
        doc["dependencies"].push_back(dependency.to_string());
    }
    if (auto written = core::write_file_atomic(sidecar_file.value(), doc.dump(2) + "\n"); !written.ok()) {
        return failure(io_failure(source_text, core::describe(written.error())));
    }
    return success(Unit{});
}

Result<Unit, ScanError> move_asset(const Path& root, const Path& source, const Path& target) {
    const std::string source_text = source.str();
    const std::string target_text = target.str();
    auto source_file = project_path(root, source_text);
    auto source_sidecar = project_path(root, source_text + std::string(kAssetMetaSuffix));
    auto target_file = project_path(root, target_text);
    auto target_sidecar = project_path(root, target_text + std::string(kAssetMetaSuffix));
    if (!source_file.ok()) {
        return failure(source_file.error());
    }
    if (!source_sidecar.ok()) {
        return failure(source_sidecar.error());
    }
    if (!target_file.ok()) {
        return failure(target_file.error());
    }
    if (!target_sidecar.ok()) {
        return failure(target_sidecar.error());
    }
    if (!core::path_exists(source_file.value()) || !core::path_exists(source_sidecar.value())) {
        return failure(scan_error(ScanErrorCode::MissingSource, source_text, "the asset has no source or no sidecar"));
    }
    if (core::path_exists(target_file.value()) || core::path_exists(target_sidecar.value())) {
        return failure(scan_error(ScanErrorCode::AlreadyExists, target_text, "the target path is already used"));
    }

    std::error_code ec;
    fs::create_directories(core::to_native_path(target_file.value()).parent_path(), ec);
    if (ec) {
        return failure(io_failure(target_text, "cannot create the target folder: " + ec.message()));
    }
    fs::rename(core::to_native_path(source_file.value()), core::to_native_path(target_file.value()), ec);
    if (ec) {
        return failure(io_failure(source_text, "cannot move the source: " + ec.message()));
    }
    fs::rename(core::to_native_path(source_sidecar.value()), core::to_native_path(target_sidecar.value()), ec);
    if (ec) {
        // Put the source back, so the asset is still registered under its old path.
        std::error_code undo;
        fs::rename(core::to_native_path(target_file.value()), core::to_native_path(source_file.value()), undo);
        return failure(io_failure(source_text, "cannot move the sidecar: " + ec.message()));
    }
    return success(Unit{});
}

Result<Unit, ScanError> remove_asset(const Path& root, const Path& source) {
    const std::string source_text = source.str();
    auto source_file = project_path(root, source_text);
    auto sidecar_file = project_path(root, source_text + std::string(kAssetMetaSuffix));
    if (!source_file.ok()) {
        return failure(source_file.error());
    }
    if (!sidecar_file.ok()) {
        return failure(sidecar_file.error());
    }
    std::error_code ec;
    if (fs::exists(core::to_native_path(source_file.value()), ec) && !fs::remove(core::to_native_path(source_file.value()), ec)) {
        return failure(io_failure(source_text, "cannot delete the source: " + (ec ? ec.message() : std::string("not removed"))));
    }
    if (fs::exists(core::to_native_path(sidecar_file.value()), ec) && !fs::remove(core::to_native_path(sidecar_file.value()), ec)) {
        return failure(io_failure(source_text, "cannot delete the sidecar: " + (ec ? ec.message() : std::string("not removed"))));
    }
    return success(Unit{});
}

}  // namespace engine::assets
