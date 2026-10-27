#include "asset_converter/converter.hpp"

#include "engine/assets/asset_project.hpp"
#include "engine/assets/asset_registry.hpp"
#include "engine/assets/content_hash.hpp"
#include "engine/assets/cooked_container.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/jobs.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace engine::converter {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;
using assets::AssetRecord;
using assets::ContentHash;
using assets::ProjectScan;
using assets::ScanError;
using assets::ScanErrorCode;
using core::Guid;
using core::Path;
using core::Result;
using core::Unit;
using core::failure;
using core::success;

constexpr std::string_view kManifestFormat = "engine.cook_manifest";
constexpr int kManifestVersion = 1;
constexpr int kRenameAttempts = 20;
constexpr int kRenameRetryMilliseconds = 10;

constexpr AssetStatus kAllStatuses[] = {AssetStatus::Valid,    AssetStatus::Imported, AssetStatus::Cooked,
                                        AssetStatus::UpToDate, AssetStatus::Failed,   AssetStatus::Blocked};

std::string hash_hex(std::string_view bytes) {
    return ContentHash::of(bytes).to_hex();
}

// A key is the digest of a canonical JSON document, so every input that affects the output must be in the document.
std::string key_of(const Json& document) {
    return hash_hex(document.dump());
}

Json nullable(const std::string& text) {
    return text.empty() ? Json(nullptr) : Json(text);
}

// A UTF-8 path as a filesystem path. A plain std::string would be read in the active code page on Windows.
fs::path native_path(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

Result<std::string, std::string> read_bytes(const std::string& utf8_path) {
    auto path = Path::from_utf8(utf8_path);
    if (!path.ok()) {
        return failure(std::string("path is not valid UTF-8: " + utf8_path));
    }
    auto bytes = core::read_file(path.value());
    if (!bytes.ok()) {
        return failure(core::describe(bytes.error()));
    }
    return success(std::move(bytes).value());
}

// The file's bytes when they hash to the expected value. A missing, unreadable, or changed file is not a match.
std::optional<std::string> read_matching(const std::string& utf8_path, const std::string& expected_hash) {
    if (expected_hash.empty()) {
        return std::nullopt;
    }
    auto bytes = read_bytes(utf8_path);
    if (!bytes.ok() || hash_hex(bytes.value()) != expected_hash) {
        return std::nullopt;
    }
    return std::move(bytes).value();
}

// Writes the bytes to a unique temporary file beside the target, then renames it over the target. Two writers never share a
// temporary file, so a concurrent writer cannot tear the target; the last rename wins, and identical inputs write identical
// bytes, so either result is the same.
Result<Unit, std::string> publish(const std::string& target, std::string_view bytes) {
    const fs::path destination = native_path(target);
    const fs::path temporary = native_path(target + "." + Guid::generate().to_string() + ".tmp");
    std::error_code error;
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file) {
            static_cast<void>(fs::remove(temporary, error));
            return failure(std::string("cannot write " + target));
        }
    }
    // Windows refuses to replace a file that another process has open without delete sharing. A few short retries let that
    // reader finish; the rename itself stays atomic.
    for (int attempt = 1;; ++attempt) {
        fs::rename(temporary, destination, error);
        if (!error || attempt == kRenameAttempts) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kRenameRetryMilliseconds));
    }
    if (error) {
        std::error_code ignored;
        static_cast<void>(fs::remove(temporary, ignored));
        return failure(std::string("cannot replace " + target + ": " + error.message()));
    }
    return success();
}

struct Manifest {
    std::string import_key;
    std::string imported_hash;
    std::string cook_key;  // empty when the manifest records no cook, for example after an import-only run
    std::string cooked_hash;
    std::vector<std::string> import_notes;  // what the importer skipped; replayed into the report when the import is reused
};

std::string string_member(const Json& document, const char* key) {
    const auto found = document.find(key);
    if (found == document.end() || !found->is_string()) {
        return {};
    }
    return found->get<std::string>();
}

// A manifest that is missing, unreadable, or from another format or version is treated as absent: the asset is converted.
std::optional<Manifest> read_manifest(const std::string& utf8_path) {
    auto text = read_bytes(utf8_path);
    if (!text.ok()) {
        return std::nullopt;
    }
    const Json document = Json::parse(text.value(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return std::nullopt;
    }
    const auto version = document.find("version");
    if (string_member(document, "format") != std::string(kManifestFormat) || version == document.end() ||
        !version->is_number_integer() || version->get<int>() != kManifestVersion) {
        return std::nullopt;
    }
    Manifest manifest{string_member(document, "import_key"), string_member(document, "imported_hash"),
                      string_member(document, "cook_key"), string_member(document, "cooked_hash"), {}};
    if (manifest.import_key.empty() || manifest.imported_hash.empty()) {
        return std::nullopt;
    }
    const auto notes = document.find("import_notes");
    if (notes == document.end() || !notes->is_array()) {
        return std::nullopt;
    }
    for (const Json& note : *notes) {
        if (!note.is_string()) {
            return std::nullopt;
        }
        manifest.import_notes.push_back(note.get<std::string>());
    }
    return manifest;
}

std::string manifest_text(const AssetRecord& record, int importer_version, const Manifest& manifest) {
    Json document = Json::object();
    document["format"] = std::string(kManifestFormat);
    document["version"] = kManifestVersion;
    document["converter_version"] = kConverterVersion;
    document["guid"] = record.guid.to_string();
    document["importer"] = record.importer;
    document["importer_version"] = importer_version;
    document["import_key"] = manifest.import_key;
    document["imported_hash"] = manifest.imported_hash;
    document["import_notes"] = Json(manifest.import_notes);
    if (!manifest.cook_key.empty()) {
        document["cook_key"] = manifest.cook_key;
        document["cooked_hash"] = manifest.cooked_hash;
    }
    return document.dump(2) + "\n";
}

Diagnostic asset_diagnostic(ErrorCode code, Severity severity, const AssetRecord& record, std::string message) {
    return Diagnostic{code, severity, record.guid.to_string(), record.source_path.str(), std::move(message)};
}

Diagnostic scan_diagnostic(const ScanError& error) {
    ErrorCode code = ErrorCode::ScanOther;
    switch (error.code) {
        case ScanErrorCode::Io:
            code = ErrorCode::ScanIo;
            break;
        case ScanErrorCode::Malformed:
            code = ErrorCode::ScanMalformed;
            break;
        case ScanErrorCode::WrongFormat:
            code = ErrorCode::ScanWrongFormat;
            break;
        case ScanErrorCode::UnsupportedVersion:
            code = ErrorCode::ScanUnsupportedVersion;
            break;
        case ScanErrorCode::GuidConflict:
            code = ErrorCode::ScanGuidConflict;
            break;
        case ScanErrorCode::PathConflict:
            code = ErrorCode::ScanPathConflict;
            break;
        case ScanErrorCode::MissingSource:
        case ScanErrorCode::AlreadyExists:
            code = ErrorCode::ScanOther;
            break;
    }
    return Diagnostic{code, Severity::Error, "", error.path, error.message};
}

// Everything the planner knows about one asset before any file is read. Assets are planned in GUID order.
struct Plan {
    const AssetRecord* record = nullptr;
    const Importer* importer = nullptr;
    AssetStatus status = AssetStatus::Valid;  // Valid: ready to convert. Failed and Blocked are final.
    std::string settings;                     // canonical settings text
    std::string import_key;
    std::string cook_key;
    std::vector<std::size_t> dependencies;    // direct dependencies that are registered, in GUID order
    std::size_t level = 0;                    // one more than the deepest dependency; a level runs after the one below
};

// Depth-first walk over the dependency graph. It produces cook keys bottom-up and blocks every asset that cannot be cooked
// because of a cycle or a failed dependency. Each asset is visited once.
class DependencyWalk {
public:
    DependencyWalk(std::vector<Plan>& plans, std::vector<Diagnostic>& diagnostics) : plans_(plans), diagnostics_(diagnostics) {
        mark_.assign(plans.size(), Mark::Unvisited);
    }

    void visit_all() {
        for (std::size_t i = 0; i < plans_.size(); ++i) {
            if (mark_[i] == Mark::Unvisited) {
                visit(i);
            }
        }
    }

    [[nodiscard]] const std::vector<std::size_t>& order() const noexcept {
        return order_;  // dependencies before the assets that use them
    }

private:
    enum class Mark { Unvisited, Active, Done };

    void block(std::size_t index, ErrorCode code, const std::string& message) {
        Plan& plan = plans_[index];
        if (plan.status != AssetStatus::Valid) {
            return;
        }
        plan.status = AssetStatus::Blocked;
        diagnostics_.push_back(asset_diagnostic(code, Severity::Error, *plan.record, message));
    }

    void visit(std::size_t index) {
        mark_[index] = Mark::Active;
        stack_.push_back(index);
        for (const std::size_t dependency : plans_[index].dependencies) {
            if (mark_[dependency] == Mark::Active) {
                // The stack holds the path from the dependency to here, so everything from it upward is in the cycle.
                const auto start = std::find(stack_.begin(), stack_.end(), dependency);
                for (auto it = start; it != stack_.end(); ++it) {
                    block(*it, ErrorCode::DependencyCycle, "is part of a dependency cycle");
                }
            } else if (mark_[dependency] == Mark::Unvisited) {
                visit(dependency);
            }
            const Plan& child = plans_[dependency];
            if (child.status != AssetStatus::Valid) {
                block(index, ErrorCode::DependencyBlocked,
                      "depends on " + child.record->guid.to_string() + ", which cannot be converted");
            }
        }

        Plan& plan = plans_[index];
        if (plan.status == AssetStatus::Valid) {
            Json dependencies = Json::array();
            for (const std::size_t dependency : plan.dependencies) {
                Json item = Json::object();
                item["guid"] = plans_[dependency].record->guid.to_string();
                item["cook_key"] = plans_[dependency].cook_key;
                dependencies.push_back(item);
                plan.level = std::max(plan.level, plans_[dependency].level + 1);
            }
            Json cook = Json::object();
            cook["converter"] = kConverterVersion;
            cook["stage"] = "cook";
            cook["import_key"] = plan.import_key;
            cook["dependencies"] = dependencies;
            plan.cook_key = key_of(cook);
        }

        mark_[index] = Mark::Done;
        stack_.pop_back();
        order_.push_back(index);
    }

    std::vector<Plan>& plans_;
    std::vector<Diagnostic>& diagnostics_;
    std::vector<Mark> mark_;
    std::vector<std::size_t> stack_;
    std::vector<std::size_t> order_;
};

void fail(Plan& plan, ErrorCode code, const std::string& message, std::vector<Diagnostic>& diagnostics) {
    plan.status = AssetStatus::Failed;
    diagnostics.push_back(asset_diagnostic(code, Severity::Error, *plan.record, message));
}

std::vector<Plan> plan_assets(const std::vector<const AssetRecord*>& records, const ImporterRegistry& importers,
                              std::vector<Diagnostic>& diagnostics, std::vector<std::size_t>& order) {
    std::vector<Plan> plans(records.size());
    std::map<Guid, std::size_t> index_of;
    for (std::size_t i = 0; i < records.size(); ++i) {
        plans[i].record = records[i];
        index_of[records[i]->guid] = i;
    }

    for (std::size_t i = 0; i < records.size(); ++i) {
        Plan& plan = plans[i];
        const AssetRecord& record = *plan.record;

        plan.importer = importers.find(record.importer);
        if (plan.importer == nullptr) {
            fail(plan, ErrorCode::UnknownImporter, "no importer named '" + record.importer + "'", diagnostics);
        } else if (auto settings = plan.importer->canonical_settings(record.importer_settings); settings.ok()) {
            plan.settings = settings.value();
        } else {
            fail(plan, ErrorCode::InvalidSettings, settings.error(), diagnostics);
        }

        if (plan.status == AssetStatus::Valid) {
            Json document = Json::object();
            document["converter"] = kConverterVersion;
            document["stage"] = "import";
            document["guid"] = record.guid.to_string();
            document["importer"] = record.importer;
            document["importer_version"] = plan.importer->version();
            document["settings"] = plan.settings;
            document["source_hash"] = record.content_hash.to_hex();
            plan.import_key = key_of(document);
        }

        for (const Guid& dependency : record.dependencies) {
            const auto found = index_of.find(dependency);
            if (found == index_of.end()) {
                fail(plan, ErrorCode::MissingDependency, "depends on " + dependency.to_string() + ", which is not registered",
                     diagnostics);
                continue;
            }
            plan.dependencies.push_back(found->second);
        }
    }

    DependencyWalk walk(plans, diagnostics);
    walk.visit_all();
    order = walk.order();
    return plans;
}

// Converts one asset in place. It writes only that asset's files, so tasks in one wave never share an output. A failure
// publishes nothing new for this asset and leaves its previous files and manifest as they were.
void convert_asset(Command command, const std::string& project, const std::string& output, const Plan& plan,
                   AssetOutcome& outcome, std::vector<Diagnostic>& diagnostics) {
    const AssetRecord& record = *plan.record;
    const std::string base = output + "/" + record.guid.to_string();
    const std::string manifest_path = base + ".manifest.json";
    const std::string imported_path = base + ".imported";
    const std::string cooked_path = base + ".cooked";
    const std::optional<Manifest> manifest = read_manifest(manifest_path);
    const auto report_failure = [&](ErrorCode code, const std::string& message) {
        outcome.status = AssetStatus::Failed;
        diagnostics.push_back(asset_diagnostic(code, Severity::Error, record, message));
    };
    const auto report_notes = [&](const std::vector<std::string>& notes) {
        for (const std::string& note : notes) {
            diagnostics.push_back(asset_diagnostic(ErrorCode::ImportUnsupported, Severity::Warning, record, note));
        }
    };

    if (command == Command::Cook && manifest && manifest->cook_key == plan.cook_key &&
        read_matching(cooked_path, manifest->cooked_hash)) {
        outcome.imported_hash = manifest->imported_hash;
        outcome.cooked_hash = manifest->cooked_hash;
        outcome.status = AssetStatus::UpToDate;
        report_notes(manifest->import_notes);
        return;
    }

    std::optional<std::string> payload;
    std::vector<std::string> notes;
    if (manifest && manifest->import_key == plan.import_key) {
        payload = read_matching(imported_path, manifest->imported_hash);
        if (payload) {
            notes = manifest->import_notes;
        }
    }
    const bool import_reused = payload.has_value();
    if (!import_reused) {
        auto source = read_bytes(project + "/" + record.source_path.str());
        if (!source.ok()) {
            report_failure(ErrorCode::SourceReadFailed, source.error());
            return;
        }
        if (ContentHash::of(source.value()) != record.content_hash) {
            report_failure(ErrorCode::SourceChanged, "the source changed after the project scan; run the command again");
            return;
        }
        auto imported = plan.importer->import(source.value(), plan.settings);
        if (!imported.ok()) {
            report_failure(ErrorCode::ImportFailed, imported.error());
            return;
        }
        ImportOutput imported_output = std::move(imported).value();
        payload = std::move(imported_output.payload);
        notes = std::move(imported_output.unsupported);
        if (auto written = publish(imported_path, *payload); !written.ok()) {
            report_failure(ErrorCode::PublishFailed, written.error());
            return;
        }
    }
    report_notes(notes);
    outcome.imported_hash = hash_hex(*payload);

    if (command == Command::Import) {
        if (import_reused) {
            outcome.status = AssetStatus::UpToDate;
            return;
        }
        // The manifest now describes the new import. Any cooked file from an earlier cook no longer matches it, so the cook
        // fields are dropped and the next cook writes a fresh file.
        const Manifest updated{plan.import_key, outcome.imported_hash, {}, {}, notes};
        if (auto written = publish(manifest_path, manifest_text(record, plan.importer->version(), updated)); !written.ok()) {
            report_failure(ErrorCode::PublishFailed, written.error());
            return;
        }
        outcome.status = AssetStatus::Imported;
        return;
    }

    const std::string cooked = engine::assets::write_cooked_container(*payload);
    if (auto written = publish(cooked_path, cooked); !written.ok()) {
        report_failure(ErrorCode::PublishFailed, written.error());
        return;
    }
    const Manifest updated{plan.import_key, outcome.imported_hash, plan.cook_key, hash_hex(cooked), notes};
    if (auto written = publish(manifest_path, manifest_text(record, plan.importer->version(), updated)); !written.ok()) {
        report_failure(ErrorCode::PublishFailed, written.error());
        return;
    }
    outcome.cooked_hash = updated.cooked_hash;
    outcome.status = AssetStatus::Cooked;
}

}  // namespace

std::string_view to_string(Command command) noexcept {
    switch (command) {
        case Command::Validate:
            return "validate";
        case Command::Import:
            return "import";
        case Command::Cook:
            return "cook";
    }
    return "unknown";
}

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::ScanIo:
            return "SCAN_IO";
        case ErrorCode::ScanMalformed:
            return "SCAN_MALFORMED";
        case ErrorCode::ScanWrongFormat:
            return "SCAN_WRONG_FORMAT";
        case ErrorCode::ScanUnsupportedVersion:
            return "SCAN_UNSUPPORTED_VERSION";
        case ErrorCode::ScanGuidConflict:
            return "SCAN_GUID_CONFLICT";
        case ErrorCode::ScanPathConflict:
            return "SCAN_PATH_CONFLICT";
        case ErrorCode::ScanOther:
            return "SCAN_OTHER";
        case ErrorCode::OrphanMetadata:
            return "ORPHAN_METADATA";
        case ErrorCode::UnregisteredSource:
            return "UNREGISTERED_SOURCE";
        case ErrorCode::UnknownImporter:
            return "UNKNOWN_IMPORTER";
        case ErrorCode::InvalidSettings:
            return "INVALID_SETTINGS";
        case ErrorCode::SourceReadFailed:
            return "SOURCE_READ_FAILED";
        case ErrorCode::SourceChanged:
            return "SOURCE_CHANGED";
        case ErrorCode::ImportFailed:
            return "IMPORT_FAILED";
        case ErrorCode::MissingDependency:
            return "MISSING_DEPENDENCY";
        case ErrorCode::DependencyCycle:
            return "DEPENDENCY_CYCLE";
        case ErrorCode::DependencyBlocked:
            return "DEPENDENCY_BLOCKED";
        case ErrorCode::PublishFailed:
            return "PUBLISH_FAILED";
        case ErrorCode::ImportUnsupported:
            return "IMPORT_UNSUPPORTED";
        case ErrorCode::InternalError:
            return "INTERNAL_ERROR";
        case ErrorCode::Cancelled:
            return "CANCELLED";
    }
    return "UNKNOWN";
}

std::string_view to_string(Severity severity) noexcept {
    return severity == Severity::Error ? "error" : "warning";
}

std::string_view to_string(AssetStatus status) noexcept {
    switch (status) {
        case AssetStatus::Valid:
            return "valid";
        case AssetStatus::Imported:
            return "imported";
        case AssetStatus::Cooked:
            return "cooked";
        case AssetStatus::UpToDate:
            return "up_to_date";
        case AssetStatus::Failed:
            return "failed";
        case AssetStatus::Blocked:
            return "blocked";
    }
    return "unknown";
}

bool Report::ok() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(),
                        [](const Diagnostic& diagnostic) { return diagnostic.severity == Severity::Error; });
}

Result<Report, std::string> run(Command command, const Options& options, const ImporterRegistry& importers) {
    if (command != Command::Validate && !options.output) {
        return failure(std::string(to_string(command)) + " needs an output directory");
    }

    Report report{command, options.project.str(), std::nullopt, {}, {}};
    if (options.output) {
        report.output = options.output->str();
    }

    auto scan = assets::scan_project(options.project);
    if (!scan.ok()) {
        report.diagnostics.push_back(scan_diagnostic(scan.error()));
        return success(std::move(report));
    }
    const ProjectScan& project = scan.value();
    for (const std::string& source : project.unregistered) {
        report.diagnostics.push_back(
            Diagnostic{ErrorCode::UnregisteredSource, Severity::Warning, "", source, "no sidecar; the source is not converted"});
    }
    for (const std::string& sidecar : project.orphan_metadata) {
        report.diagnostics.push_back(
            Diagnostic{ErrorCode::OrphanMetadata, Severity::Warning, "", sidecar, "the source is missing; the sidecar is ignored"});
    }

    const std::vector<const AssetRecord*> records = project.registry.records();
    std::vector<std::size_t> order;
    std::vector<Plan> plans = plan_assets(records, importers, report.diagnostics, order);
    report.assets.resize(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        AssetOutcome& outcome = report.assets[i];
        outcome.guid = records[i]->guid.to_string();
        outcome.source = records[i]->source_path.str();
        outcome.importer = records[i]->importer;
        outcome.status = plans[i].status;
        outcome.import_key = plans[i].import_key;
        outcome.cook_key = plans[i].cook_key;
    }
    if (command == Command::Validate) {
        return success(std::move(report));
    }

    const std::string project_text = options.project.str();
    const std::string output = options.output->str();
    if (auto created = core::create_directories(*options.output); !created.ok()) {
        report.diagnostics.push_back(Diagnostic{ErrorCode::PublishFailed, Severity::Error, "", output,
                                                 "cannot create the output directory: " + core::describe(created.error())});
        for (AssetOutcome& outcome : report.assets) {
            if (outcome.status == AssetStatus::Valid) {
                outcome.status = AssetStatus::Failed;
            }
        }
        return success(std::move(report));
    }

    // Cook waits for dependencies, so the assets run in waves by level. Import has no dependencies and runs as one wave.
    std::size_t deepest = 0;
    for (const Plan& plan : plans) {
        deepest = std::max(deepest, command == Command::Cook ? plan.level : std::size_t{0});
    }
    std::vector<std::vector<Diagnostic>> job_diagnostics(records.size());
    const auto cancel_requested = [&options]() {
        return options.cancel != nullptr && options.cancel->load(std::memory_order_acquire);
    };
    const auto count_done = [&options]() {
        if (options.completed != nullptr) {
            options.completed->fetch_add(1, std::memory_order_acq_rel);
        }
    };
    // An asset that is not valid is finished as far as progress goes: the plan has already failed it or blocked it.
    for (const Plan& plan : plans) {
        if (plan.status != AssetStatus::Valid) {
            count_done();
        }
    }
    core::JobSystem jobs(options.jobs);
    for (std::size_t wave = 0; wave <= deepest; ++wave) {
        // A cancel stops before the next wave. The assets already published stay; the rest keep status Valid.
        if (cancel_requested()) {
            break;
        }
        core::TaskGroup group = jobs.create_group();
        for (std::size_t i = 0; i < plans.size(); ++i) {
            const Plan& plan = plans[i];
            const std::size_t level = command == Command::Cook ? plan.level : std::size_t{0};
            if (plan.status != AssetStatus::Valid || level != wave) {
                continue;
            }
            bool dependency_failed = false;
            if (command == Command::Cook) {
                for (const std::size_t dependency : plan.dependencies) {
                    const AssetStatus status = report.assets[dependency].status;
                    dependency_failed = dependency_failed || status == AssetStatus::Failed || status == AssetStatus::Blocked;
                }
            }
            if (dependency_failed) {
                report.assets[i].status = AssetStatus::Blocked;
                job_diagnostics[i].push_back(
                    asset_diagnostic(ErrorCode::DependencyBlocked, Severity::Error, *plan.record,
                                     "a dependency failed or was blocked, so the asset was not cooked"));
                count_done();
                continue;
            }
            static_cast<void>(group.submit([&, i](const core::CancellationToken&) {
                // A cancel that arrives after the wave started still stops the asset before it reads anything.
                if (cancel_requested()) {
                    return;
                }
                try {
                    convert_asset(command, project_text, output, plans[i], report.assets[i], job_diagnostics[i]);
                } catch (const std::exception& error) {
                    report.assets[i].status = AssetStatus::Failed;
                    job_diagnostics[i].push_back(asset_diagnostic(ErrorCode::InternalError, Severity::Error, *plans[i].record,
                                                                  error.what()));
                }
                count_done();
            }));
        }
        group.wait();
    }

    for (const std::vector<Diagnostic>& diagnostics : job_diagnostics) {
        report.diagnostics.insert(report.diagnostics.end(), diagnostics.begin(), diagnostics.end());
    }
    // Valid after a run means the asset was never reached, so a cancel left it unconverted.
    const std::size_t untouched = static_cast<std::size_t>(std::count_if(
        report.assets.begin(), report.assets.end(), [](const AssetOutcome& outcome) { return outcome.status == AssetStatus::Valid; }));
    if (cancel_requested() && untouched > 0) {
        report.cancelled = true;
        report.diagnostics.push_back(Diagnostic{ErrorCode::Cancelled, Severity::Error, "", "",
                                                "the run was cancelled; " + std::to_string(untouched) + " asset(s) were not converted"});
    }
    return success(std::move(report));
}

std::string report_to_json(const Report& report) {
    Json document = Json::object();
    document["format"] = std::string(kReportFormat);
    document["version"] = kReportVersion;
    document["converter_version"] = kConverterVersion;
    document["command"] = std::string(to_string(report.command));
    document["project"] = report.project;
    document["output"] = report.output ? Json(*report.output) : Json(nullptr);
    document["status"] = report.ok() ? "ok" : "failed";

    Json counts = Json::object();
    for (const AssetStatus status : kAllStatuses) {
        counts[std::string(to_string(status))] = 0;
    }
    for (const AssetOutcome& outcome : report.assets) {
        counts[std::string(to_string(outcome.status))] = counts[std::string(to_string(outcome.status))].get<int>() + 1;
    }
    document["counts"] = counts;

    Json assets = Json::array();
    for (const AssetOutcome& outcome : report.assets) {
        Json item = Json::object();
        item["guid"] = outcome.guid;
        item["source"] = outcome.source;
        item["importer"] = outcome.importer;
        item["status"] = std::string(to_string(outcome.status));
        item["import_key"] = nullable(outcome.import_key);
        item["cook_key"] = nullable(outcome.cook_key);
        item["imported_hash"] = nullable(outcome.imported_hash);
        item["cooked_hash"] = nullable(outcome.cooked_hash);
        assets.push_back(item);
    }
    document["assets"] = assets;

    Json diagnostics = Json::array();
    for (const Diagnostic& diagnostic : report.diagnostics) {
        Json item = Json::object();
        item["code"] = std::string(to_string(diagnostic.code));
        item["severity"] = std::string(to_string(diagnostic.severity));
        item["asset"] = nullable(diagnostic.asset);
        item["source"] = nullable(diagnostic.source);
        item["message"] = diagnostic.message;
        diagnostics.push_back(item);
    }
    document["diagnostics"] = diagnostics;
    return document.dump(2) + "\n";
}

}  // namespace engine::converter
