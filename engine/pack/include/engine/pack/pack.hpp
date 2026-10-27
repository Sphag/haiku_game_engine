#pragma once

// Resource packs (US-060): the cooked payloads of one or more roots and everything they depend on, in one file the
// standalone runtime reads without the converter or the source project.
//
// File layout, little-endian:
//   magic "OWPK" (4 bytes), u32 format version, u64 index size, 32-byte SHA-256 of the index text, then the index, then
//   the payloads. Each payload is the full "<guid>.cooked" file of one asset, so the runtime reads it with
//   read_cooked_container().
//
// The index is canonical JSON (nlohmann keeps keys sorted, and entries are in GUID order, so identical inputs produce
// identical bytes):
//
//   {
//     "format": "engine.pack",
//     "version": 1,
//     "roots": [ "<guid>", ... ],                 // the assets the pack was built for, in GUID order
//     "entries": [
//       { "guid": "<guid>", "source": "scenes/start.json", "hash": "<sha256 hex>",
//         "offset": 0, "size": 123, "dependencies": [ "<guid>", ... ] }
//     ]
//   }
//
// "hash" is the SHA-256 of the entry's payload. It is the asset's version in the pack: a rebuilt asset with the same bytes keeps
// its hash. "offset" counts from the first payload byte.
//
// Dependency closure: a pack holds every asset reachable from its roots through dependency edges, and nothing else. A root
// or dependency that is not in the registry, or that has no cooked file, aborts the build with a MissingDependency or
// MissingCooked error, so a pack never ships with a hole in it.
//
// Integrity: PackReader::open checks the header, the index hash, the JSON, every payload hash, every payload range, and that
// every dependency of every entry is an entry of the pack. A damaged or incomplete file is rejected as a whole.

#include "engine/assets/asset_registry.hpp"

#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace engine::pack {

inline constexpr std::string_view kPackMagic = "OWPK";
inline constexpr std::string_view kPackFormat = "engine.pack";
inline constexpr std::uint32_t kPackVersion = 1;
inline constexpr std::size_t kPackHeaderBytes = 4 + 4 + 8 + 32;  // magic, version, index size, index hash
inline constexpr std::string_view kPackSuffix = ".pack";

enum class PackErrorCode {
    Io,                  // a file could not be read or written
    NoRoots,             // the build was given no roots
    UnknownRoot,         // a root is not a registered asset
    MissingDependency,   // a reachable dependency is not a registered asset
    MissingCooked,       // a reachable asset has no cooked payload
    Truncated,           // the file ends before the header, the index, or a payload
    WrongFormat,         // the magic or the format name is not a pack
    UnsupportedVersion,  // the format version is not kPackVersion
    Malformed,           // the index is not JSON, or a member is missing, mistyped, duplicated, or out of range
    IntegrityFailed,     // the index hash or a payload hash does not match
    IncompleteClosure,   // an entry depends on an asset the pack does not hold
};

[[nodiscard]] std::string_view to_string(PackErrorCode code) noexcept;

struct PackError {
    PackErrorCode code;
    std::string guid;     // the asset the error concerns, or empty when none applies
    std::string message;  // what went wrong, for the log
};

// Supplies the cooked file bytes of one asset. A failure carries a message.
using CookedLookup = std::function<core::Result<std::string, std::string>(const core::Guid&)>;

// Builds the pack bytes for the closure of the roots. Fails before producing any bytes when a root or a reachable dependency
// is missing from the registry or has no cooked payload.
[[nodiscard]] core::Result<std::string, PackError> build_pack(const assets::AssetRegistry& registry,
                                                             const std::vector<core::Guid>& roots,
                                                             const CookedLookup& cooked);

struct PackEntry {
    core::Guid guid;
    std::string source;  // project-relative source path, for diagnostics
    std::string hash;    // lowercase SHA-256 hex of the payload
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::vector<core::Guid> dependencies;  // sorted, unique
};

// An opened, fully verified pack. The bytes are owned, so the payload views the reader returns stay valid while it lives.
class PackReader {
public:
    // Verifies the whole pack. On failure nothing is returned.
    [[nodiscard]] static core::Result<PackReader, PackError> open(std::string bytes);

    [[nodiscard]] const std::vector<core::Guid>& roots() const noexcept {
        return roots_;
    }

    [[nodiscard]] const std::vector<PackEntry>& entries() const noexcept {
        return entries_;
    }

    [[nodiscard]] const PackEntry* find(const core::Guid& guid) const;

    // The cooked file bytes of one entry, which the caller passes to read_cooked_container().
    [[nodiscard]] std::string_view payload(const PackEntry& entry) const;

private:
    PackReader() = default;

    std::string bytes_;
    std::size_t payload_start_ = 0;
    std::vector<core::Guid> roots_;
    std::vector<PackEntry> entries_;
    std::map<core::Guid, std::size_t> index_;  // GUID to position in entries_
};

// Reads a pack file and verifies it. A read failure is reported as Io.
[[nodiscard]] core::Result<PackReader, PackError> open_pack_file(const core::Path& path);

// Writes the pack bytes with core::write_file_atomic. Any existing pack at the path is replaced only on success.
[[nodiscard]] core::Result<core::Unit, PackError> write_pack_file(const core::Path& path, std::string_view bytes);

}  // namespace engine::pack
