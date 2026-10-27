#include "engine/assets/asset_registry.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>

namespace engine::assets {
namespace {

using core::Guid;
using core::Path;
using core::Result;
using core::Unit;
using core::failure;
using core::success;

RegistryError error(RegistryErrorCode code, std::string message) {
    return RegistryError{code, std::move(message)};
}

std::optional<RegistryError> check_path(const Path& source_path) {
    // Path normalizes an empty text to ".", which names the project root and no source.
    if (source_path.str() == "." || source_path.is_absolute()) {
        return error(RegistryErrorCode::InvalidPath, "source path must name a file under the project root: '" + source_path.str() + "'");
    }
    return std::nullopt;
}

}  // namespace

Result<Unit, RegistryError> AssetRegistry::add(AssetRecord record) {
    if (record.guid.is_nil()) {
        return failure(error(RegistryErrorCode::NilGuid, "an asset has the nil GUID"));
    }
    if (const auto problem = check_path(record.source_path)) {
        return failure(*problem);
    }
    if (record.importer.empty()) {
        return failure(error(RegistryErrorCode::EmptyImporter, "asset " + record.guid.to_string() + " has no importer"));
    }
    if (records_.contains(record.guid)) {
        return failure(error(RegistryErrorCode::GuidConflict, "GUID " + record.guid.to_string() + " is already registered"));
    }
    if (by_path_.contains(record.source_path.str())) {
        return failure(error(RegistryErrorCode::PathConflict, "source path '" + record.source_path.str() + "' is already registered"));
    }

    std::sort(record.dependencies.begin(), record.dependencies.end());
    record.dependencies.erase(std::unique(record.dependencies.begin(), record.dependencies.end()), record.dependencies.end());
    if (std::binary_search(record.dependencies.begin(), record.dependencies.end(), record.guid)) {
        return failure(error(RegistryErrorCode::SelfDependency, "asset " + record.guid.to_string() + " depends on itself"));
    }

    by_path_.emplace(record.source_path.str(), record.guid);
    const Guid key = record.guid;
    records_.emplace(key, std::move(record));
    return success(Unit{});
}

Result<std::vector<Guid>, RegistryError> AssetRegistry::remove(const Guid& guid) {
    const auto found = records_.find(guid);
    if (found == records_.end()) {
        return failure(error(RegistryErrorCode::NotFound, "no asset has GUID " + guid.to_string()));
    }
    std::vector<Guid> affected = dependents(guid);
    by_path_.erase(found->second.source_path.str());
    records_.erase(found);
    return success(std::move(affected));
}

Result<Unit, RegistryError> AssetRegistry::move(const Guid& guid, const Path& new_path) {
    const auto found = records_.find(guid);
    if (found == records_.end()) {
        return failure(error(RegistryErrorCode::NotFound, "no asset has GUID " + guid.to_string()));
    }
    if (const auto problem = check_path(new_path)) {
        return failure(*problem);
    }
    const auto taken = by_path_.find(new_path.str());
    if (taken != by_path_.end() && taken->second != guid) {
        return failure(error(RegistryErrorCode::PathConflict, "source path '" + new_path.str() + "' is already registered"));
    }

    by_path_.erase(found->second.source_path.str());
    found->second.source_path = new_path;
    by_path_.emplace(new_path.str(), guid);
    return success(Unit{});
}

const AssetRecord* AssetRegistry::find(const Guid& guid) const {
    const auto found = records_.find(guid);
    return found == records_.end() ? nullptr : &found->second;
}

const AssetRecord* AssetRegistry::find_by_path(const Path& source_path) const {
    const auto found = by_path_.find(source_path.str());
    return found == by_path_.end() ? nullptr : find(found->second);
}

std::vector<const AssetRecord*> AssetRegistry::records() const {
    std::vector<const AssetRecord*> result;
    result.reserve(records_.size());
    for (const auto& entry : records_) {
        result.push_back(&entry.second);
    }
    return result;
}

std::vector<Guid> AssetRegistry::dependents(const Guid& guid) const {
    std::vector<Guid> result;
    for (const auto& entry : records_) {
        const auto& dependencies = entry.second.dependencies;
        if (std::binary_search(dependencies.begin(), dependencies.end(), guid)) {
            result.push_back(entry.first);
        }
    }
    return result;
}

std::vector<MissingDependency> AssetRegistry::missing_dependencies() const {
    std::vector<MissingDependency> result;
    for (const auto& entry : records_) {
        for (const Guid& dependency : entry.second.dependencies) {
            if (!records_.contains(dependency)) {
                result.push_back(MissingDependency{entry.first, dependency});
            }
        }
    }
    return result;
}

std::vector<Guid> AssetRegistry::dependency_closure(const Guid& guid) const {
    std::set<Guid> reached;
    std::vector<Guid> pending{guid};
    while (!pending.empty()) {
        const Guid current = pending.back();
        pending.pop_back();
        const AssetRecord* record = find(current);
        if (record == nullptr) {
            continue;
        }
        for (const Guid& dependency : record->dependencies) {
            if (reached.insert(dependency).second) {
                pending.push_back(dependency);
            }
        }
    }
    reached.erase(guid);
    return std::vector<Guid>(reached.begin(), reached.end());
}

}  // namespace engine::assets
