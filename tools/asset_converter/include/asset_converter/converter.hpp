#pragma once

// Asset converter (US-019): validates, imports, and cooks a project headlessly. The command-line tool is a thin wrapper
// over run(); the editor and the tests call the same entry point.
//
// Output layout, for each registered asset with GUID <guid>, in the output directory:
//   <guid>.imported        the importer's payload
//   <guid>.cooked          the cooked container: magic "OWCK", format version, payload size, payload
//   <guid>.manifest.json   the cache record: the keys and content hashes of the two files above
//
// Caching. An import is reused when its key matches the manifest and the imported file still has the recorded hash. The
// import key covers the converter version, the importer type and version, the canonical settings, and the source hash. A
// cook is reused when its key matches and the cooked file still has the recorded hash. The cook key covers the import key
// and the cook keys of the direct dependencies, so a change anywhere below an asset invalidates it.
//
// Publishing. Each file is written to a unique temporary name beside its target and then renamed over it. Parallel tasks
// and concurrent processes never share a temporary file, and identical inputs produce identical bytes. A failed import or
// cook writes nothing, so the previous valid output stays in place.
//
// Failures are per asset. A failed asset is reported with an error code; an asset that depends on a failed, invalid, or
// cyclic asset is blocked and is not converted.

#include "asset_converter/importer.hpp"

#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::converter {

// Version of the cooked container and of the key layout. Every cache key includes it.
inline constexpr int kConverterVersion = 1;
inline constexpr std::string_view kReportFormat = "engine.converter_report";
inline constexpr int kReportVersion = 1;

enum class Command {
    Validate,  // scan and check sidecars, importers, settings, and dependencies; write nothing
    Import,    // run importers into <guid>.imported
    Cook,      // import what is stale, then write <guid>.cooked
};
[[nodiscard]] std::string_view to_string(Command command) noexcept;

// Stable machine-readable codes. The text of each code is part of the report format: add codes, never rename them.
enum class ErrorCode {
    ScanIo,                // the project tree or a sidecar could not be read
    ScanMalformed,         // a sidecar is not valid JSON, has an unknown key, or has a bad value
    ScanWrongFormat,       // a sidecar's format name is not engine.asset_meta
    ScanUnsupportedVersion,
    ScanGuidConflict,      // two sources claim the same GUID
    ScanPathConflict,      // two sidecars name the same source path
    ScanOther,
    OrphanMetadata,        // warning: a sidecar whose source is missing; it is not converted
    UnregisteredSource,    // warning: a source without a sidecar; it is not converted
    UnknownImporter,       // the sidecar names an importer that is not registered
    InvalidSettings,       // the importer rejects the settings
    SourceReadFailed,      // the source file could not be read
    SourceChanged,         // the source changed after the project scan, so its bytes no longer match the registry
    ImportFailed,          // the importer rejected the source bytes
    ImportUnsupported,     // warning: the importer skipped data it does not support; the rest of the asset is converted
    MissingDependency,     // a dependency names a GUID that is not registered
    DependencyCycle,       // the asset is part of a dependency cycle
    DependencyBlocked,     // a dependency failed, is invalid, or is blocked
    PublishFailed,         // an output file or the output directory could not be written
    InternalError,         // a conversion task threw an unexpected exception
    Cancelled,             // the run was cancelled before every asset was converted; the rest were not converted
};
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

enum class Severity { Warning, Error };
[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

struct Diagnostic {
    ErrorCode code;
    Severity severity;
    std::string asset;    // GUID, or empty when the diagnostic is not about one asset
    std::string source;   // project-relative source path, or empty
    std::string message;
};

enum class AssetStatus {
    Valid,     // validate: the asset passes every check
    Imported,  // import: the importer ran and <guid>.imported was written
    Cooked,    // cook: <guid>.cooked was written
    UpToDate,  // the cached import or cook was reused
    Failed,    // the asset has its own error
    Blocked,   // a dependency has an error, so the asset was not converted
};
[[nodiscard]] std::string_view to_string(AssetStatus status) noexcept;

struct AssetOutcome {
    std::string guid;
    std::string source;
    std::string importer;
    AssetStatus status = AssetStatus::Failed;
    std::string import_key;   // hex; empty when the key could not be computed
    std::string cook_key;     // hex; empty when the asset is blocked or invalid
    std::string imported_hash;
    std::string cooked_hash;
};

struct Report {
    Command command;
    std::string project;
    std::optional<std::string> output;  // absent for validate
    std::vector<AssetOutcome> assets;   // GUID order
    std::vector<Diagnostic> diagnostics;
    bool cancelled = false;  // the run stopped early because Options::cancel was set

    // True when no diagnostic has error severity. Warnings do not fail a run.
    [[nodiscard]] bool ok() const noexcept;
};

struct Options {
    core::Path project;
    std::optional<core::Path> output;  // required for import and cook
    std::size_t jobs = 0;              // parallel conversion workers; 0 uses the hardware thread count
    // Cancellation (US-066). The flag is read before each asset starts; an asset that has started finishes and publishes, so the
    // output directory never holds a half-written file. Not owned; the caller keeps it alive for the run.
    const std::atomic<bool>* cancel = nullptr;
    // Progress (US-066). Incremented once for each asset that has been converted, cached, or failed. Not owned.
    std::atomic<std::size_t>* completed = nullptr;
};

// Runs one command. A usage problem, such as import or cook without an output directory, fails with a message. Everything
// else, including failed assets and a project that cannot be scanned, is in the returned report.
[[nodiscard]] core::Result<Report, std::string> run(Command command, const Options& options, const ImporterRegistry& importers);

// A PNG or JPEG decoded for an editor thumbnail: RGBA8, scaled down so neither side is longer than max_side.
struct TexturePreview {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;  // width * height * 4 bytes, row by row from the top
};

// Decodes a texture source for display. It accepts the same files as the texture importer and reports the same failures.
[[nodiscard]] core::Result<TexturePreview, std::string> make_texture_preview(std::string_view bytes, std::uint32_t max_side);

// The report as JSON text: format and version, the command, the status, counts by asset status, the assets, and the
// diagnostics with their codes. Keys are sorted, and the text ends with a newline.
[[nodiscard]] std::string report_to_json(const Report& report);

}  // namespace engine::converter
