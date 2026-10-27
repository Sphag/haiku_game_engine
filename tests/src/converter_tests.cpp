// Asset converter (US-019): import, cook, validate, the machine-readable report, incremental reuse, determinism, and safe
// publishing. Every case works in a scratch project and output directory, so the checked-in fixture is never modified. These
// tests touch the file system, so they are integration tests; the importers themselves are unit tests.

#include "asset_converter/converter.hpp"
#include "asset_converter/importer.hpp"

#include "engine/assets/content_hash.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using engine::assets::ContentHash;
using engine::converter::AssetOutcome;
using engine::converter::AssetStatus;
using engine::converter::Command;
using engine::converter::Diagnostic;
using engine::converter::ErrorCode;
using engine::converter::Importer;
using engine::converter::ImporterRegistry;
using engine::converter::ImportOutput;
using engine::converter::Options;
using engine::converter::Report;
using engine::core::Guid;
using engine::core::Path;
using engine::core::Result;
namespace fs = std::filesystem;

namespace {

// Fixed GUIDs for the checked-in fixture: text (notes), blob (data), and json (config, which depends on notes).
constexpr const char* kNotesGuid = "3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b00";
constexpr const char* kBlobGuid = "3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b01";
constexpr const char* kConfigGuid = "3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b02";

// Guids for the hand-built projects in the failure cases.
constexpr const char* kMissingUserGuid = "4a3b2c1d-0e9f-4a8b-9c7d-6e5f4a3b2c10";
constexpr const char* kCycleAGuid = "4a3b2c1d-0e9f-4a8b-9c7d-6e5f4a3b2c11";
constexpr const char* kCycleBGuid = "4a3b2c1d-0e9f-4a8b-9c7d-6e5f4a3b2c12";
constexpr const char* kDependentGuid = "4a3b2c1d-0e9f-4a8b-9c7d-6e5f4a3b2c13";
constexpr const char* kPlainBlobGuid = "4a3b2c1d-0e9f-4a8b-9c7d-6e5f4a3b2c14";
constexpr const char* kUnknownGuid = "4a3b2c1d-0e9f-4a8b-9c7d-6e5f4a3b2c15";
constexpr const char* kBadSettingsGuid = "4a3b2c1d-0e9f-4a8b-9c7d-6e5f4a3b2c16";
constexpr const char* kMissingGuid = "4a3b2c1d-0e9f-4a8b-9c7d-6e5f4a3b2c99";
constexpr const char* kModelGuid = "4a3b2c1d-0e9f-4a8b-9c7d-6e5f4a3b2c20";
constexpr const char* kBrokenModelGuid = "4a3b2c1d-0e9f-4a8b-9c7d-6e5f4a3b2c21";

std::string utf8(const fs::path& value) {
    const auto text = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string read_file_text(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

void write_file(const fs::path& file, const std::string& contents) {
    fs::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << contents;
}

// A scratch directory, removed when the case ends.
class ScratchDir {
public:
    ScratchDir() : root_(fs::temp_directory_path() / ("engine_converter_" + Guid::generate().to_string())) {
        fs::create_directories(root_);
    }
    ~ScratchDir() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept {
        return root_;
    }

private:
    fs::path root_;
};

// A copy of the checked-in fixture project in a scratch directory.
fs::path copy_fixture(const ScratchDir& scratch) {
    const fs::path source = fs::path(std::string(ENGINE_TEST_FIXTURE_DIR)) / "converter" / "project";
    const fs::path project = scratch.path() / "project";
    fs::copy(source, project, fs::copy_options::recursive);
    return project;
}

// Writes "<source>.meta" in the engine.asset_meta format. `settings` is JSON text; `dependencies` are GUID strings.
void write_sidecar(const fs::path& project, const std::string& source, const std::string& guid, const std::string& importer,
                   const std::string& settings, const std::vector<std::string>& dependencies) {
    std::string listed;
    for (std::size_t i = 0; i < dependencies.size(); ++i) {
        listed += (i == 0 ? "\"" : ", \"") + dependencies[i] + "\"";
    }
    write_file(project / (source + ".meta"),
               "{\n  \"format\": \"engine.asset_meta\",\n  \"version\": 1,\n  \"guid\": \"" + guid +
                   "\",\n  \"importer\": {\"type\": \"" + importer + "\", \"settings\": " + settings + "},\n  \"dependencies\": [" +
                   listed + "]\n}\n");
}

Options options_for(const fs::path& project, const std::optional<fs::path>& output = std::nullopt, std::size_t jobs = 0) {
    std::optional<Path> out;
    if (output) {
        out = Path::from_utf8(utf8(*output)).value();
    }
    return Options{Path::from_utf8(utf8(project)).value(), out, jobs};
}

Report run_checked(Command command, const Options& options, const ImporterRegistry& importers = ImporterRegistry::builtin()) {
    auto result = engine::converter::run(command, options, importers);
    REQUIRE(result.ok());
    return std::move(result).value();
}

// The outcome for a GUID. A missing asset fails the check that asked for it and gives a default outcome.
AssetOutcome outcome_for(const Report& report, const char* guid) {
    for (const AssetOutcome& outcome : report.assets) {
        if (outcome.guid == guid) {
            return outcome;
        }
    }
    INFO("no asset " << guid << " in the report");
    CHECK(false);
    return AssetOutcome{};
}

bool has_code(const Report& report, ErrorCode code) {
    return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                       [code](const Diagnostic& diagnostic) { return diagnostic.code == code; });
}

// Same importer behavior, a different version: the cache must treat every asset of this type as changed.
class VersionedImporter final : public Importer {
public:
    VersionedImporter(std::unique_ptr<Importer> inner, int version) : inner_(std::move(inner)), version_(version) {}
    [[nodiscard]] std::string_view type() const noexcept override {
        return inner_->type();
    }
    [[nodiscard]] int version() const noexcept override {
        return version_;
    }
    [[nodiscard]] Result<std::string, std::string> canonical_settings(std::string_view settings_json) const override {
        return inner_->canonical_settings(settings_json);
    }
    [[nodiscard]] Result<ImportOutput, std::string> import(std::string_view source, std::string_view settings) const override {
        return inner_->import(source, settings);
    }

private:
    std::unique_ptr<Importer> inner_;
    int version_;
};

}  // namespace

TEST_CASE("Converter: the text importer checks UTF-8 and normalizes line endings only when asked", "[unit]") {
    const auto text = engine::converter::make_text_importer();
    CHECK(text->canonical_settings("{}").value() == "{\"normalize_newlines\":false}");
    CHECK(text->canonical_settings("{ \"normalize_newlines\": false }").value() == "{\"normalize_newlines\":false}");
    CHECK(!text->canonical_settings("{\"normalize_newlines\": 1}").ok());
    CHECK(!text->canonical_settings("{\"bogus\": true}").ok());
    CHECK(!text->canonical_settings("[]").ok());

    CHECK(text->import("a\r\nb", "{\"normalize_newlines\":true}").value().payload == "a\nb");
    CHECK(text->import("a\r\nb", "{\"normalize_newlines\":false}").value().payload == "a\r\nb");
    CHECK(text->import("lone\rcr", "{\"normalize_newlines\":true}").value().payload == "lone\rcr");
    CHECK(!text->import("\xff\xfe", "{\"normalize_newlines\":false}").ok());
}

TEST_CASE("Converter: the json importer re-emits canonical JSON and rejects malformed text", "[unit]") {
    const auto json = engine::converter::make_json_importer();
    const std::string spaced = "{ \"b\": 2,\n  \"a\": [1, 2.5] }";
    const std::string compact = "{\"a\":[1,2.5],\"b\":2}";
    CHECK(json->import(spaced, "{}").value().payload == compact);
    CHECK(json->import(compact, "{}").value().payload == compact);
    CHECK(!json->import("{\"a\":", "{}").ok());
    CHECK(!json->canonical_settings("{\"x\": 1}").ok());
}

TEST_CASE("Converter: the registry rejects empty and duplicate importer types", "[unit]") {
    ImporterRegistry registry;
    CHECK(registry.add(engine::converter::make_blob_importer()).ok());
    CHECK(!registry.add(engine::converter::make_blob_importer()).ok());
    CHECK(registry.find("blob") != nullptr);
    CHECK(registry.find("texture") == nullptr);
    CHECK(!registry.add(nullptr).ok());
}

TEST_CASE("Converter: the fixture cooks into one container per asset, with text normalized", "[integration]") {
    ScratchDir scratch;
    const fs::path project = copy_fixture(scratch);
    const fs::path output = scratch.path() / "out";

    const Report report = run_checked(Command::Cook, options_for(project, output));
    CHECK(report.ok());
    CHECK(report.assets.size() == 3);
    for (const char* guid : {kNotesGuid, kBlobGuid, kConfigGuid}) {
        CHECK(outcome_for(report, guid).status == AssetStatus::Cooked);
        const std::string cooked = read_file_text(output / (std::string(guid) + ".cooked"));
        CHECK(cooked.substr(0, 4) == "OWCK");
        CHECK(fs::exists(output / (std::string(guid) + ".manifest.json")));
    }
    CHECK(read_file_text(output / (std::string(kNotesGuid) + ".imported")) == "Line one\nLine two\n");
    CHECK(read_file_text(output / (std::string(kConfigGuid) + ".imported")) == "{\"a\":[1,2.5,\"x\"],\"b\":2}");
}

TEST_CASE("Converter: a second cook reuses every cached result when nothing changed", "[integration]") {
    ScratchDir scratch;
    const fs::path project = copy_fixture(scratch);
    const fs::path output = scratch.path() / "out";
    const Report first = run_checked(Command::Cook, options_for(project, output));
    REQUIRE(first.ok());

    const Report second = run_checked(Command::Cook, options_for(project, output));
    CHECK(second.ok());
    for (const AssetOutcome& outcome : second.assets) {
        CHECK(outcome.status == AssetStatus::UpToDate);
    }
    for (std::size_t i = 0; i < first.assets.size(); ++i) {
        CHECK(second.assets[i].cooked_hash == first.assets[i].cooked_hash);
    }
}

TEST_CASE("Converter: identical inputs give identical output hashes, whatever the worker count", "[integration]") {
    ScratchDir scratch;
    const fs::path project = copy_fixture(scratch);
    const Report serial = run_checked(Command::Cook, options_for(project, scratch.path() / "serial", 1));
    const Report parallel = run_checked(Command::Cook, options_for(project, scratch.path() / "parallel", 8));
    REQUIRE(serial.assets.size() == parallel.assets.size());
    for (std::size_t i = 0; i < serial.assets.size(); ++i) {
        CHECK(serial.assets[i].guid == parallel.assets[i].guid);
        CHECK(serial.assets[i].import_key == parallel.assets[i].import_key);
        CHECK(serial.assets[i].cook_key == parallel.assets[i].cook_key);
        CHECK(serial.assets[i].imported_hash == parallel.assets[i].imported_hash);
        CHECK(serial.assets[i].cooked_hash == parallel.assets[i].cooked_hash);
    }
    CHECK(read_file_text(scratch.path() / "serial" / (std::string(kConfigGuid) + ".cooked")) ==
          read_file_text(scratch.path() / "parallel" / (std::string(kConfigGuid) + ".cooked")));
}

TEST_CASE("Converter: a changed setting re-cooks the asset and everything that depends on it", "[integration]") {
    ScratchDir scratch;
    const fs::path project = copy_fixture(scratch);
    const fs::path output = scratch.path() / "out";
    REQUIRE(run_checked(Command::Cook, options_for(project, output)).ok());

    write_sidecar(project, "notes/readme.txt", kNotesGuid, "text", "{\"normalize_newlines\": false}", {});
    const Report report = run_checked(Command::Cook, options_for(project, output));
    CHECK(report.ok());
    CHECK(outcome_for(report, kNotesGuid).status == AssetStatus::Cooked);
    CHECK(outcome_for(report, kConfigGuid).status == AssetStatus::Cooked);
    CHECK(outcome_for(report, kBlobGuid).status == AssetStatus::UpToDate);
    CHECK(read_file_text(output / (std::string(kNotesGuid) + ".imported")) == "Line one\r\nLine two\r\n");
}

TEST_CASE("Converter: settings that differ only in spelling or defaults share one cache entry", "[integration]") {
    ScratchDir scratch;
    const fs::path project = copy_fixture(scratch);
    const fs::path output = scratch.path() / "out";
    REQUIRE(run_checked(Command::Cook, options_for(project, output)).ok());

    // The fixture sets normalize_newlines to true. Writing the default spelled out must not change the canonical settings.
    write_sidecar(project, "notes/readme.txt", kNotesGuid, "text", "{}", {});
    const Report with_default = run_checked(Command::Cook, options_for(project, output));
    CHECK(outcome_for(with_default, kNotesGuid).status == AssetStatus::Cooked);
    write_sidecar(project, "notes/readme.txt", kNotesGuid, "text", "{ \"normalize_newlines\": false }", {});
    const Report spelled_out = run_checked(Command::Cook, options_for(project, output));
    CHECK(outcome_for(spelled_out, kNotesGuid).status == AssetStatus::UpToDate);
}

TEST_CASE("Converter: a tool version change re-cooks every asset of that importer and its dependents only", "[integration]") {
    ScratchDir scratch;
    const fs::path project = copy_fixture(scratch);
    const fs::path output = scratch.path() / "out";
    REQUIRE(run_checked(Command::Cook, options_for(project, output)).ok());

    ImporterRegistry bumped;
    REQUIRE(bumped.add(engine::converter::make_blob_importer()).ok());
    REQUIRE(bumped.add(std::make_unique<VersionedImporter>(engine::converter::make_text_importer(), 2)).ok());
    REQUIRE(bumped.add(engine::converter::make_json_importer()).ok());

    const Report report = run_checked(Command::Cook, options_for(project, output), bumped);
    CHECK(outcome_for(report, kNotesGuid).status == AssetStatus::Cooked);
    CHECK(outcome_for(report, kConfigGuid).status == AssetStatus::Cooked);
    CHECK(outcome_for(report, kBlobGuid).status == AssetStatus::UpToDate);
}

TEST_CASE("Converter: a failed import keeps the previous valid result and blocks its dependents", "[integration]") {
    ScratchDir scratch;
    const fs::path project = copy_fixture(scratch);
    const fs::path output = scratch.path() / "out";
    REQUIRE(run_checked(Command::Cook, options_for(project, output)).ok());

    const fs::path cooked = output / (std::string(kNotesGuid) + ".cooked");
    const fs::path imported = output / (std::string(kNotesGuid) + ".imported");
    const fs::path manifest = output / (std::string(kNotesGuid) + ".manifest.json");
    const std::string cooked_before = read_file_text(cooked);
    const std::string imported_before = read_file_text(imported);
    const std::string manifest_before = read_file_text(manifest);

    write_file(project / "notes" / "readme.txt", std::string("\xff\xfe broken", 9));
    const Report report = run_checked(Command::Cook, options_for(project, output));
    CHECK(!report.ok());
    CHECK(outcome_for(report, kNotesGuid).status == AssetStatus::Failed);
    CHECK(outcome_for(report, kConfigGuid).status == AssetStatus::Blocked);
    CHECK(outcome_for(report, kBlobGuid).status == AssetStatus::UpToDate);
    CHECK(has_code(report, ErrorCode::ImportFailed));
    CHECK(has_code(report, ErrorCode::DependencyBlocked));

    CHECK(read_file_text(cooked) == cooked_before);
    CHECK(read_file_text(imported) == imported_before);
    CHECK(read_file_text(manifest) == manifest_before);
}

TEST_CASE("Converter: missing dependencies, cycles, and their dependents are reported and not converted", "[integration]") {
    ScratchDir scratch;
    const fs::path project = scratch.path() / "project";
    const fs::path output = scratch.path() / "out";
    write_file(project / "user.bin", "needs something missing");
    write_sidecar(project, "user.bin", kMissingUserGuid, "blob", "{}", {kMissingGuid});
    write_file(project / "cycle_a.bin", "a");
    write_sidecar(project, "cycle_a.bin", kCycleAGuid, "blob", "{}", {kCycleBGuid});
    write_file(project / "cycle_b.bin", "b");
    write_sidecar(project, "cycle_b.bin", kCycleBGuid, "blob", "{}", {kCycleAGuid});
    write_file(project / "dependent.bin", "uses the cycle");
    write_sidecar(project, "dependent.bin", kDependentGuid, "blob", "{}", {kCycleAGuid});
    write_file(project / "plain.bin", "stands alone");
    write_sidecar(project, "plain.bin", kPlainBlobGuid, "blob", "{}", {});

    const Report validated = run_checked(Command::Validate, options_for(project));
    CHECK(!validated.ok());
    CHECK(outcome_for(validated, kMissingUserGuid).status == AssetStatus::Failed);
    CHECK(outcome_for(validated, kCycleAGuid).status == AssetStatus::Blocked);
    CHECK(outcome_for(validated, kCycleBGuid).status == AssetStatus::Blocked);
    CHECK(outcome_for(validated, kDependentGuid).status == AssetStatus::Blocked);
    CHECK(outcome_for(validated, kPlainBlobGuid).status == AssetStatus::Valid);
    CHECK(has_code(validated, ErrorCode::MissingDependency));
    CHECK(has_code(validated, ErrorCode::DependencyCycle));

    const Report cooked = run_checked(Command::Cook, options_for(project, output));
    CHECK(!cooked.ok());
    CHECK(outcome_for(cooked, kPlainBlobGuid).status == AssetStatus::Cooked);
    CHECK(outcome_for(cooked, kDependentGuid).status == AssetStatus::Blocked);
    CHECK(!fs::exists(output / (std::string(kCycleAGuid) + ".cooked")));
    CHECK(!fs::exists(output / (std::string(kDependentGuid) + ".cooked")));
    CHECK(fs::exists(output / (std::string(kPlainBlobGuid) + ".cooked")));
}

TEST_CASE("Converter: an unknown importer or rejected settings fail the asset before anything is written", "[integration]") {
    ScratchDir scratch;
    const fs::path project = scratch.path() / "project";
    const fs::path output = scratch.path() / "out";
    write_file(project / "mystery.tex", "pixels");
    write_sidecar(project, "mystery.tex", kUnknownGuid, "no_such_importer", "{}", {});
    write_file(project / "bad.txt", "words");
    write_sidecar(project, "bad.txt", kBadSettingsGuid, "text", "{\"bogus\": true}", {});

    const Report report = run_checked(Command::Cook, options_for(project, output));
    CHECK(!report.ok());
    CHECK(outcome_for(report, kUnknownGuid).status == AssetStatus::Failed);
    CHECK(outcome_for(report, kBadSettingsGuid).status == AssetStatus::Failed);
    CHECK(has_code(report, ErrorCode::UnknownImporter));
    CHECK(has_code(report, ErrorCode::InvalidSettings));
    CHECK(!fs::exists(output / (std::string(kUnknownGuid) + ".cooked")));
    CHECK(!fs::exists(output / (std::string(kBadSettingsGuid) + ".cooked")));
}

TEST_CASE("Converter: a malformed sidecar fails the scan with its code and converts nothing", "[integration]") {
    ScratchDir scratch;
    const fs::path project = copy_fixture(scratch);
    const fs::path output = scratch.path() / "out";
    write_file(project / "config" / "settings.json.meta", "{ not json");

    const Report report = run_checked(Command::Cook, options_for(project, output));
    CHECK(!report.ok());
    CHECK(report.assets.empty());
    CHECK(has_code(report, ErrorCode::ScanMalformed));
    CHECK(!fs::exists(output));
}

TEST_CASE("Converter: a missing project fails with a scan code in the report", "[integration]") {
    ScratchDir scratch;
    const Report report = run_checked(Command::Validate, options_for(scratch.path() / "does_not_exist"));
    CHECK(!report.ok());
    CHECK(has_code(report, ErrorCode::ScanIo));
}

TEST_CASE("Converter: a command that writes needs an output directory", "[integration]") {
    ScratchDir scratch;
    const fs::path project = copy_fixture(scratch);
    auto result = engine::converter::run(Command::Import, options_for(project), ImporterRegistry::builtin());
    REQUIRE(!result.ok());
    CHECK(result.error() == "import needs an output directory");
}

TEST_CASE("Converter: import writes imports without cooks, and a later cook adds them", "[integration]") {
    ScratchDir scratch;
    const fs::path project = copy_fixture(scratch);
    const fs::path output = scratch.path() / "out";

    const Report imported = run_checked(Command::Import, options_for(project, output));
    CHECK(imported.ok());
    CHECK(outcome_for(imported, kBlobGuid).status == AssetStatus::Imported);
    CHECK(fs::exists(output / (std::string(kBlobGuid) + ".imported")));
    CHECK(!fs::exists(output / (std::string(kBlobGuid) + ".cooked")));

    const Report cooked = run_checked(Command::Cook, options_for(project, output));
    CHECK(cooked.ok());
    CHECK(outcome_for(cooked, kBlobGuid).status == AssetStatus::Cooked);
    CHECK(outcome_for(cooked, kBlobGuid).imported_hash == outcome_for(imported, kBlobGuid).imported_hash);
}

TEST_CASE("Converter: concurrent cooks of one output directory leave every file complete and identical", "[integration]") {
    ScratchDir scratch;
    const fs::path project = scratch.path() / "project";
    for (int i = 0; i < 48; ++i) {
        const std::string source = "bulk/item_" + std::to_string(i) + ".bin";
        write_file(project / source, std::string(static_cast<std::size_t>(1024 + i * 37), static_cast<char>('a' + i % 26)));
        write_sidecar(project, source, Guid::generate().to_string(), "blob", "{}", {});
    }
    const fs::path reference = scratch.path() / "reference";
    const Report expected = run_checked(Command::Cook, options_for(project, reference, 1));
    REQUIRE(expected.ok());

    const fs::path shared = scratch.path() / "shared";
    const ImporterRegistry importers = ImporterRegistry::builtin();
    std::optional<Report> reports[2];
    std::thread workers[2];
    for (std::size_t t = 0; t < 2; ++t) {
        workers[t] = std::thread([&, t] {
            auto result = engine::converter::run(Command::Cook, options_for(project, shared, 6), importers);
            if (result.ok()) {
                reports[t] = std::move(result).value();
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    REQUIRE(reports[0].has_value());
    REQUIRE(reports[1].has_value());
    CHECK(reports[0]->ok());
    CHECK(reports[1]->ok());

    for (const AssetOutcome& outcome : expected.assets) {
        const std::string name = outcome.guid + ".cooked";
        CHECK(read_file_text(shared / name) == read_file_text(reference / name));
    }
    const Report after = run_checked(Command::Cook, options_for(project, shared, 6));
    for (const AssetOutcome& outcome : after.assets) {
        CHECK(outcome.status == AssetStatus::UpToDate);
    }
}

TEST_CASE("Converter: the JSON report names the format, the status, the asset states, and the error codes", "[integration]") {
    ScratchDir scratch;
    const fs::path project = scratch.path() / "project";
    write_file(project / "user.bin", "x");
    write_sidecar(project, "user.bin", kMissingUserGuid, "blob", "{}", {kMissingGuid});

    const Report report = run_checked(Command::Validate, options_for(project));
    const std::string json = engine::converter::report_to_json(report);
    CHECK(json.find("\"format\": \"engine.converter_report\"") != std::string::npos);
    CHECK(json.find("\"version\": 1") != std::string::npos);
    CHECK(json.find("\"command\": \"validate\"") != std::string::npos);
    CHECK(json.find("\"output\": null") != std::string::npos);
    CHECK(json.find("\"status\": \"failed\"") != std::string::npos);
    CHECK(json.find("\"code\": \"MISSING_DEPENDENCY\"") != std::string::npos);
    CHECK(json.find("\"severity\": \"error\"") != std::string::npos);
    CHECK(json.find("\"status\": \"failed\"") != std::string::npos);
}

TEST_CASE("Converter: model notes are reported again when the cached import is reused", "[integration]") {
    ScratchDir scratch;
    const fs::path project = scratch.path() / "project";
    write_file(project / "triangle.obj", "mtllib missing.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Paint\nf 1 2 3\n");
    write_sidecar(project, "triangle.obj", kModelGuid, "model", "{}", {});
    const fs::path output = scratch.path() / "out";

    const Report first = run_checked(Command::Import, options_for(project, output));
    CHECK(outcome_for(first, kModelGuid).status == AssetStatus::Imported);
    CHECK(has_code(first, ErrorCode::ImportUnsupported));

    const Report second = run_checked(Command::Import, options_for(project, output));
    CHECK(outcome_for(second, kModelGuid).status == AssetStatus::UpToDate);
    CHECK(has_code(second, ErrorCode::ImportUnsupported));
    CHECK(second.ok());  // a note is a warning, so the run still succeeds
}

TEST_CASE("Converter: a source that is not a model fails, and no output is written for it", "[integration]") {
    ScratchDir scratch;
    const fs::path project = scratch.path() / "project";
    write_file(project / "broken.obj", "this is plain text, not a model\n");
    write_sidecar(project, "broken.obj", kBrokenModelGuid, "model", "{}", {});
    const fs::path output = scratch.path() / "out";

    const Report report = run_checked(Command::Cook, options_for(project, output));
    CHECK(outcome_for(report, kBrokenModelGuid).status == AssetStatus::Failed);
    CHECK(has_code(report, ErrorCode::ImportFailed));
    CHECK(!fs::exists(output / (std::string(kBrokenModelGuid) + ".imported")));
    CHECK(!fs::exists(output / (std::string(kBrokenModelGuid) + ".cooked")));
    CHECK(!fs::exists(output / (std::string(kBrokenModelGuid) + ".manifest.json")));
}
