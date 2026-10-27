#include "engine/pack/pack.hpp"

#include "engine/assets/content_hash.hpp"
#include "engine/core/file_io.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <set>
#include <utility>

namespace engine::pack {

namespace {

using assets::AssetRecord;
using assets::AssetRegistry;
using assets::ContentHash;
using core::Guid;
using core::Path;
using core::Result;
using core::Unit;

PackError make_error(PackErrorCode code, std::string guid, std::string message) {
    return PackError{code, std::move(guid), std::move(message)};
}

void append_little_endian(std::string& out, std::uint64_t value, int byte_count) {
    for (int i = 0; i < byte_count; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
    }
}

std::uint64_t read_little_endian(std::string_view bytes, std::size_t offset, int byte_count) {
    std::uint64_t value = 0;
    for (int i = 0; i < byte_count; ++i) {
        const auto byte = static_cast<std::uint8_t>(bytes[offset + static_cast<std::size_t>(i)]);
        value |= static_cast<std::uint64_t>(byte) << (8 * i);
    }
    return value;
}

std::string hash_bytes_of(const ContentHash& hash) {
    const auto& bytes = hash.bytes();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// A JSON string member, or nullopt when the member is missing or not a string.
std::optional<std::string> string_member(const nlohmann::json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) {
        return std::nullopt;
    }
    return object.at(key).get<std::string>();
}

std::optional<std::uint64_t> unsigned_member(const nlohmann::json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_number_unsigned()) {
        return std::nullopt;
    }
    return object.at(key).get<std::uint64_t>();
}

// Parses a GUID array member. Any non-string or unparsable item fails.
std::optional<std::vector<Guid>> guid_array(const nlohmann::json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_array()) {
        return std::nullopt;
    }
    std::vector<Guid> guids;
    for (const auto& item : object.at(key)) {
        if (!item.is_string()) {
            return std::nullopt;
        }
        auto parsed = Guid::parse(item.get<std::string>());
        if (!parsed.ok()) {
            return std::nullopt;
        }
        guids.push_back(parsed.value());
    }
    return guids;
}

}  // namespace

std::string_view to_string(PackErrorCode code) noexcept {
    switch (code) {
        case PackErrorCode::Io: return "Io";
        case PackErrorCode::NoRoots: return "NoRoots";
        case PackErrorCode::UnknownRoot: return "UnknownRoot";
        case PackErrorCode::MissingDependency: return "MissingDependency";
        case PackErrorCode::MissingCooked: return "MissingCooked";
        case PackErrorCode::Truncated: return "Truncated";
        case PackErrorCode::WrongFormat: return "WrongFormat";
        case PackErrorCode::UnsupportedVersion: return "UnsupportedVersion";
        case PackErrorCode::Malformed: return "Malformed";
        case PackErrorCode::IntegrityFailed: return "IntegrityFailed";
        case PackErrorCode::IncompleteClosure: return "IncompleteClosure";
    }
    return "Unknown";
}

Result<std::string, PackError> build_pack(const AssetRegistry& registry, const std::vector<Guid>& roots,
                                          const CookedLookup& cooked) {
    if (roots.empty()) {
        return core::failure(make_error(PackErrorCode::NoRoots, "", "a pack needs at least one root"));
    }

    // Walk the closure from the roots. Each reached asset must be registered; the parent is named in the message.
    std::set<Guid> closure;
    std::vector<Guid> pending;
    for (const auto& root : roots) {
        if (registry.find(root) == nullptr) {
            return core::failure(make_error(PackErrorCode::UnknownRoot, root.to_string(), "root is not a registered asset"));
        }
        pending.push_back(root);
    }
    while (!pending.empty()) {
        const Guid guid = pending.back();
        pending.pop_back();
        if (!closure.insert(guid).second) {
            continue;
        }
        const AssetRecord* record = registry.find(guid);
        for (const auto& dependency : record->dependencies) {
            if (registry.find(dependency) == nullptr) {
                return core::failure(make_error(PackErrorCode::MissingDependency, dependency.to_string(),
                                                "dependency of " + guid.to_string() + " is not a registered asset"));
            }
            pending.push_back(dependency);
        }
    }

    // Payloads go in GUID order (the set's order), so the output does not depend on the order of the roots.
    std::string payload;
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& guid : closure) {
        const AssetRecord* record = registry.find(guid);
        auto bytes = cooked(guid);
        if (!bytes.ok()) {
            return core::failure(make_error(PackErrorCode::MissingCooked, guid.to_string(), bytes.error()));
        }
        const std::string& cooked_bytes = bytes.value();
        const std::string hash = ContentHash::of(cooked_bytes).to_hex();

        nlohmann::json dependencies = nlohmann::json::array();
        for (const auto& dependency : record->dependencies) {
            dependencies.push_back(dependency.to_string());
        }
        entries.push_back({
            {"guid", guid.to_string()},
            {"source", record->source_path.str()},
            {"hash", hash},
            {"offset", payload.size()},
            {"size", cooked_bytes.size()},
            {"dependencies", dependencies},
        });
        payload.append(cooked_bytes);
    }

    nlohmann::json root_list = nlohmann::json::array();
    for (const auto& root : std::set<Guid>(roots.begin(), roots.end())) {
        root_list.push_back(root.to_string());
    }
    const nlohmann::json index = {
        {"format", std::string(kPackFormat)},
        {"version", kPackVersion},
        {"roots", root_list},
        {"entries", entries},
    };
    const std::string index_text = index.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);

    std::string out(kPackMagic);
    append_little_endian(out, kPackVersion, 4);
    append_little_endian(out, index_text.size(), 8);
    out.append(hash_bytes_of(ContentHash::of(index_text)));
    out.append(index_text);
    out.append(payload);
    return core::success(std::move(out));
}

Result<PackReader, PackError> PackReader::open(std::string bytes) {
    const std::string_view view(bytes);
    if (view.size() < kPackHeaderBytes) {
        return core::failure(make_error(PackErrorCode::Truncated, "",
                                        "pack is " + std::to_string(view.size()) + " bytes, header needs " +
                                            std::to_string(kPackHeaderBytes)));
    }
    if (view.substr(0, kPackMagic.size()) != kPackMagic) {
        return core::failure(make_error(PackErrorCode::WrongFormat, "", "not a resource pack: magic is not \"OWPK\""));
    }
    const auto version = read_little_endian(view, 4, 4);
    if (version != kPackVersion) {
        return core::failure(make_error(PackErrorCode::UnsupportedVersion, "",
                                        "unsupported pack version " + std::to_string(version)));
    }
    const auto index_size = read_little_endian(view, 8, 8);
    const auto available = view.size() - kPackHeaderBytes;
    if (index_size > available) {
        return core::failure(make_error(PackErrorCode::Truncated, "",
                                        "index needs " + std::to_string(index_size) + " bytes, file has " +
                                            std::to_string(available)));
    }
    const auto index_text = view.substr(kPackHeaderBytes, static_cast<std::size_t>(index_size));
    const auto expected_hash = view.substr(16, 32);
    if (hash_bytes_of(ContentHash::of(index_text)) != expected_hash) {
        return core::failure(make_error(PackErrorCode::IntegrityFailed, "", "index hash does not match the header"));
    }

    const auto index = nlohmann::json::parse(index_text, nullptr, false);
    if (index.is_discarded() || !index.is_object()) {
        return core::failure(make_error(PackErrorCode::Malformed, "", "index is not a JSON object"));
    }
    if (string_member(index, "format") != std::string(kPackFormat)) {
        return core::failure(make_error(PackErrorCode::WrongFormat, "", "index format is not engine.pack"));
    }
    const auto index_version = unsigned_member(index, "version");
    if (!index_version || *index_version != kPackVersion) {
        return core::failure(make_error(PackErrorCode::UnsupportedVersion, "", "index version is not supported"));
    }

    PackReader reader;
    reader.bytes_ = std::move(bytes);
    reader.payload_start_ = kPackHeaderBytes + static_cast<std::size_t>(index_size);
    const std::size_t payload_size = reader.bytes_.size() - reader.payload_start_;

    auto roots = guid_array(index, "roots");
    if (!roots) {
        return core::failure(make_error(PackErrorCode::Malformed, "", "roots must be an array of GUIDs"));
    }
    reader.roots_ = std::move(*roots);

    if (!index.contains("entries") || !index.at("entries").is_array()) {
        return core::failure(make_error(PackErrorCode::Malformed, "", "entries must be an array"));
    }
    std::uint64_t next_offset = 0;
    for (const auto& item : index.at("entries")) {
        auto guid_text = string_member(item, "guid");
        auto source = string_member(item, "source");
        auto hash = string_member(item, "hash");
        auto offset = unsigned_member(item, "offset");
        auto size = unsigned_member(item, "size");
        auto dependencies = guid_array(item, "dependencies");
        if (!guid_text || !source || !hash || !offset || !size || !dependencies) {
            return core::failure(make_error(PackErrorCode::Malformed, "", "entry is missing a member or has the wrong type"));
        }
        auto guid = Guid::parse(*guid_text);
        if (!guid.ok()) {
            return core::failure(make_error(PackErrorCode::Malformed, *guid_text, "entry GUID is not valid"));
        }
        if (reader.index_.contains(guid.value())) {
            return core::failure(make_error(PackErrorCode::Malformed, *guid_text, "entry GUID appears twice"));
        }
        // Payloads are stored back to back in entry order, so a gap or an overlap means the index does not describe the file.
        if (*offset != next_offset || *size > payload_size - next_offset) {
            return core::failure(make_error(PackErrorCode::Truncated, *guid_text, "payload range does not fit the file"));
        }
        next_offset += *size;

        PackEntry entry;
        entry.guid = guid.value();
        entry.source = std::move(*source);
        entry.hash = std::move(*hash);
        entry.offset = *offset;
        entry.size = *size;
        entry.dependencies = std::move(*dependencies);
        const auto payload = reader.payload(entry);
        if (ContentHash::of(payload).to_hex() != entry.hash) {
            return core::failure(make_error(PackErrorCode::IntegrityFailed, *guid_text, "payload hash does not match the index"));
        }
        reader.index_.emplace(entry.guid, reader.entries_.size());
        reader.entries_.push_back(std::move(entry));
    }
    if (next_offset != payload_size) {
        return core::failure(make_error(PackErrorCode::Truncated, "",
                                        "entries cover " + std::to_string(next_offset) + " of " +
                                            std::to_string(payload_size) + " payload bytes"));
    }

    // The closure is part of the pack's contract: every root and every dependency of every entry must be held here.
    for (const auto& root : reader.roots_) {
        if (!reader.index_.contains(root)) {
            return core::failure(make_error(PackErrorCode::IncompleteClosure, root.to_string(), "root has no entry"));
        }
    }
    for (const auto& entry : reader.entries_) {
        for (const auto& dependency : entry.dependencies) {
            if (!reader.index_.contains(dependency)) {
                return core::failure(make_error(PackErrorCode::IncompleteClosure, dependency.to_string(),
                                                "dependency of " + entry.guid.to_string() + " is not in the pack"));
            }
        }
    }
    return core::success(std::move(reader));
}

const PackEntry* PackReader::find(const Guid& guid) const {
    const auto found = index_.find(guid);
    return found == index_.end() ? nullptr : &entries_[found->second];
}

std::string_view PackReader::payload(const PackEntry& entry) const {
    return std::string_view(bytes_).substr(payload_start_ + static_cast<std::size_t>(entry.offset),
                                           static_cast<std::size_t>(entry.size));
}

Result<PackReader, PackError> open_pack_file(const Path& path) {
    auto bytes = core::read_file(path);
    if (!bytes.ok()) {
        return core::failure(make_error(PackErrorCode::Io, "", core::describe(bytes.error())));
    }
    return PackReader::open(std::move(bytes).value());
}

Result<Unit, PackError> write_pack_file(const Path& path, std::string_view bytes) {
    auto written = core::write_file_atomic(path, bytes);
    if (!written.ok()) {
        return core::failure(make_error(PackErrorCode::Io, "", core::describe(written.error())));
    }
    return core::success(Unit{});
}

}  // namespace engine::pack
