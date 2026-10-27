#include "engine/editor/distribution.hpp"

#include "engine/assets/cooked_container.hpp"
#include "engine/assets/content_hash.hpp"
#include "engine/core/file_io.hpp"
#include "engine/pack/pack.hpp"

#include "asset_converter/converter.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace engine::editor {

namespace {

namespace fs = std::filesystem;
using core::Guid;
using core::Path;
using converter::Command;

constexpr std::size_t kMaxNameLength = 64;
constexpr std::string_view kRuntimeDir = "data";
constexpr std::string_view kPackFile = "game.pack";
constexpr std::string_view kShaderDir = "shaders";
constexpr std::string_view kNoticesFile = "NOTICES.txt";
constexpr std::string_view kStartedText = "game_runtime: started scene ";
constexpr std::string_view kCancelled = "cancelled";
constexpr std::string_view kDefaultProfile = "native";

#ifdef _WIN32
constexpr std::string_view kPlatform = "windows";
constexpr std::string_view kDefaultBackend = "dx12";
constexpr std::string_view kLauncher = "run_game.bat";
const char* const kRuntimeName = "game_runtime.exe";
#else
constexpr std::string_view kPlatform = "linux";
constexpr std::string_view kDefaultBackend = "vulkan";
constexpr std::string_view kLauncher = "run_game.sh";
const char* const kRuntimeName = "game_runtime";
#endif

// The filesystem path of a core path, and back. Paths are UTF-8 on both sides, so a name survives the round trip.
std::string utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

Path to_core(const fs::path& path) {
    return Path::from_utf8(utf8(path)).value();
}

// Joins one name under a directory. The names used here are fixed or validated, so the join cannot fail.
Path child(const Path& dir, std::string_view name) {
    return dir.join(name).value();
}

bool valid_game_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxNameLength || name.front() == '.') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}

std::string first_error(const converter::Report& report) {
    for (const auto& diagnostic : report.diagnostics) {
        if (diagnostic.severity == converter::Severity::Error) {
            return std::string(converter::to_string(diagnostic.code)) + ": " + diagnostic.asset + " " + diagnostic.message;
        }
    }
    return "the converter reported an error";
}

// Copies one file, replacing the destination. Returns the failure text, or an empty string on success.
std::string copy_one(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return "could not copy " + utf8(from) + ": " + ec.message();
    }
    return {};
}

void make_executable(const fs::path& path) {
    std::error_code ec;
    fs::permissions(path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
}

// The package's own files, relative to its root, with '/' separators, in sorted order.
std::vector<fs::path> package_files(const fs::path& root) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            files.push_back(fs::relative(entry.path(), root));
        }
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return a.generic_string() < b.generic_string(); });
    return files;
}

std::string launcher_text(const std::string& guid, const std::string& backend, const std::string& profile) {
#ifdef _WIN32
    return "@echo off\r\n"
           "\"%~dp0" + std::string(kRuntimeName) + "\" --pack \"%~dp0" + std::string(kRuntimeDir) + "\\" + std::string(kPackFile) +
           "\" --start-scene " + guid + " --backend " + backend + " --profile " + profile + " %*\r\n";
#else
    return "#!/bin/sh\n"
           "dir=$(cd \"$(dirname \"$0\")\" && pwd)\n"
           "exec \"$dir/" + std::string(kRuntimeName) + "\" --pack \"$dir/" + std::string(kRuntimeDir) + "/" + std::string(kPackFile) +
           "\" --start-scene " + guid + " --backend " + backend + " --profile " + profile + " \"$@\"\n";
#endif
}

// Runs the runtime with the directory as its working directory and its output in log. Returns the exit code, or -1 when it did
// not run. The arguments are built here from GUIDs, fixed names, and checked values, so none needs quoting.
int run_runtime(const fs::path& dir, const std::string& arguments, const fs::path& log) {
#ifdef _WIN32
    const std::string command = "cd /d \"" + utf8(dir) + "\" && \"" + kRuntimeName + "\" " + arguments + " > \"" + utf8(log) +
                                "\" 2>&1";
    return std::system(command.c_str());
#else
    const std::string command = "cd '" + utf8(dir) + "' && './" + kRuntimeName + "' " + arguments + " > '" + utf8(log) + "' 2>&1";
    const int status = std::system(command.c_str());
    if (status == -1 || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
#endif
}

// Removes a folder when the build leaves scope, unless it was published.
class ScopedRemove {
public:
    explicit ScopedRemove(Path path) : path_(std::move(path)) {}
    ~ScopedRemove() {
        if (!keep_) {
            static_cast<void>(core::remove_all(path_));
        }
    }
    ScopedRemove(const ScopedRemove&) = delete;
    ScopedRemove& operator=(const ScopedRemove&) = delete;
    void keep() noexcept {
        keep_ = true;
    }

private:
    Path path_;
    bool keep_ = false;
};

bool is_package(const Path& folder) {
    if (!core::path_exists(child(folder, kPackageManifest))) {
        return false;
    }
    auto text = core::read_text_file(child(folder, kPackageManifest));
    if (!text.ok()) {
        return false;
    }
    const auto manifest = nlohmann::json::parse(text.value(), nullptr, false);
    return manifest.is_object() && manifest.value("format", std::string()) == kPackageFormat;
}

}  // namespace

std::string_view to_string(BuildStage stage) noexcept {
    switch (stage) {
        case BuildStage::Idle:
            return "Idle";
        case BuildStage::Validate:
            return "Validate";
        case BuildStage::Cook:
            return "Cook";
        case BuildStage::Pack:
            return "Pack";
        case BuildStage::Package:
            return "Package";
        case BuildStage::Verify:
            return "Verify";
        case BuildStage::Done:
            return "Done";
    }
    return "Unknown";
}

core::Result<GameBuildReport, std::string> build_game(const GameBuildOptions& options, BuildProgress& progress,
                                                      const converter::ImporterRegistry& importers) {
    const auto set_stage = [&progress](BuildStage stage) { progress.stage.store(static_cast<std::uint8_t>(stage)); };
    const auto cancelled = [&progress]() { return progress.cancel.load(); };

    // The options hold UTF-8 text. Every path is converted once here, and a path that is not valid UTF-8 stops the build.
    const auto project_path = Path::from_utf8(options.project);
    const auto output_path = Path::from_utf8(options.output_dir);
    const auto runtime_path = Path::from_utf8(options.runtime_exe);
    const auto shader_path = Path::from_utf8(options.shader_dir);
    const auto notices_path = Path::from_utf8(options.notices_file);
    if (!project_path.ok() || !output_path.ok() || !runtime_path.ok() || !shader_path.ok() || !notices_path.ok()) {
        return core::failure(std::string("a path is not valid UTF-8 or is malformed"));
    }
    if (!valid_game_name(options.game_name)) {
        return core::failure(std::string("the game name must be 1 to 64 letters, digits, '-', '_', or '.', and not start with '.'"));
    }
    const std::string backend = options.backend.empty() ? std::string(kDefaultBackend) : options.backend;
    const std::string profile = options.profile.empty() ? std::string(kDefaultProfile) : options.profile;
    if (backend != "dx12" && backend != "vulkan") {
        return core::failure("the backend must be dx12 or vulkan, not '" + backend + "'");
    }
    if (profile != "native" && profile != "full") {
        return core::failure("the profile must be native or full, not '" + profile + "'");
    }
    const std::string guid = options.start_scene.to_string();
    if (!core::path_exists(runtime_path.value())) {
        return core::failure("the runtime executable is missing: " + options.runtime_exe);
    }
    if (!core::path_exists(notices_path.value())) {
        return core::failure("the notices file is missing: " + options.notices_file);
    }

    GameBuildReport report;
    const std::string name = options.game_name;
    const Path output = output_path.value();
    const Path build_dir = child(output, name + ".build");
    const Path cooked_dir = child(build_dir, "cooked");
    const Path staging = child(output, name + ".staging");
    const Path target = child(output, name);
    if (auto made = core::create_directories(output); !made.ok()) {
        return core::failure("could not create the output folder: " + output.str());
    }
    ScopedRemove staging_guard(staging);
    report.package_dir = target.str();

    // Validate: the converter checks every sidecar, importer, setting, and dependency. Nothing is written.
    set_stage(BuildStage::Validate);
    converter::Options run_options{project_path.value(), std::nullopt, 0, nullptr, nullptr};
    auto validation = converter::run(Command::Validate, run_options, importers);
    if (!validation.ok()) {
        return core::failure("validation: " + validation.error());
    }
    if (!validation.value().ok()) {
        return core::failure("validation failed: " + first_error(validation.value()));
    }
    auto scan = assets::scan_project(project_path.value());
    if (!scan.ok()) {
        return core::failure("project scan failed: " + scan.error().message);
    }
    const auto& registry = scan.value().registry;
    const auto* start = registry.find(options.start_scene);
    if (start == nullptr) {
        return core::failure("the start scene " + guid + " is not a registered asset");
    }
    if (start->importer != "json") {
        return core::failure("the start scene " + guid + " is not a scene: its importer is " + start->importer);
    }
    report.log.push_back("validated " + std::to_string(registry.records().size()) + " assets");
    if (cancelled()) {
        return core::failure(std::string(kCancelled));
    }

    // Cook: import what is stale and write <guid>.cooked for every asset. A cancelled cook publishes what it finished.
    set_stage(BuildStage::Cook);
    if (auto made = core::create_directories(cooked_dir); !made.ok()) {
        return core::failure("could not create the cooked folder: " + cooked_dir.str());
    }
    run_options.output = cooked_dir;
    run_options.cancel = &progress.cancel;
    run_options.completed = &progress.done;
    progress.done.store(0);
    progress.total.store(registry.records().size());
    auto cook = converter::run(Command::Cook, run_options, importers);
    if (!cook.ok()) {
        return core::failure("cook: " + cook.error());
    }
    if (cook.value().cancelled || cancelled()) {
        return core::failure(std::string(kCancelled));
    }
    if (!cook.value().ok()) {
        return core::failure("cook failed: " + first_error(cook.value()));
    }
    report.log.push_back("cooked " + std::to_string(registry.records().size()) + " assets");

    // Pack: the start scene and every asset it reaches, and nothing else. A missing dependency aborts the build.
    set_stage(BuildStage::Pack);
    const auto cooked = [&cooked_dir](const Guid& guid_value) -> core::Result<std::string, std::string> {
        auto path = cooked_dir.join(guid_value.to_string() + ".cooked");
        if (!path.ok()) {
            return core::failure(std::string("cooked file name is not a valid path"));
        }
        return assets::read_cooked_file(path.value());
    };
    auto bytes = pack::build_pack(registry, {options.start_scene}, cooked);
    if (!bytes.ok()) {
        const auto& error = bytes.error();
        return core::failure("pack: " + std::string(pack::to_string(error.code)) + " " + error.guid + " " + error.message);
    }
    auto reader = pack::PackReader::open(bytes.value());
    if (!reader.ok()) {
        return core::failure("the built pack failed its own check: " + reader.error().message);
    }
    report.pack_entries = reader.value().entries().size();
    report.log.push_back("packed " + std::to_string(report.pack_entries) + " assets for the start scene");
    if (cancelled()) {
        return core::failure(std::string(kCancelled));
    }

    // Package: assemble the staging folder. The target folder is replaced only when it is a package or does not exist.
    set_stage(BuildStage::Package);
    if (core::path_exists(target) && !is_package(target)) {
        return core::failure("the folder " + target.str() + " exists and is not a game package; choose another name or folder");
    }
    static_cast<void>(core::remove_all(staging));
    const Path pack_dir = child(staging, kRuntimeDir);
    const Path shader_out = child(staging, kShaderDir);
    if (auto made = core::create_directories(pack_dir); !made.ok()) {
        return core::failure("could not create " + pack_dir.str());
    }
    if (auto made = core::create_directories(shader_out); !made.ok()) {
        return core::failure("could not create " + shader_out.str());
    }
    if (auto written = pack::write_pack_file(child(pack_dir, kPackFile), bytes.value()); !written.ok()) {
        return core::failure("pack: " + written.error().message);
    }

    const fs::path shader_src = core::to_native_path(shader_path.value());
    std::size_t shader_count = 0;
    std::error_code ec;
    if (fs::is_directory(shader_src, ec)) {
        for (const auto& entry : fs::directory_iterator(shader_src)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string failure_text = copy_one(entry.path(), core::to_native_path(child(shader_out, utf8(entry.path().filename()))));
            if (!failure_text.empty()) {
                return core::failure(failure_text);
            }
            ++shader_count;
        }
    }
    if (shader_count == 0) {
        return core::failure("no compiled shaders in " + options.shader_dir + "; build the shader toolchain first");
    }
    report.log.push_back("copied " + std::to_string(shader_count) + " shaders");

    const Path runtime_out = child(staging, kRuntimeName);
    if (auto failed = copy_one(core::to_native_path(runtime_path.value()), core::to_native_path(runtime_out)); !failed.empty()) {
        return core::failure(failed);
    }
    make_executable(core::to_native_path(runtime_out));
    if (auto failed = copy_one(core::to_native_path(notices_path.value()), core::to_native_path(child(staging, kNoticesFile)));
        !failed.empty()) {
        return core::failure(failed);
    }
    if (auto written = core::write_file_atomic(child(staging, kLauncher), launcher_text(guid, backend, profile)); !written.ok()) {
        return core::failure("could not write the launcher: " + written.error().path);
    }
    make_executable(core::to_native_path(child(staging, kLauncher)));

    // The manifest records the settings the launcher uses and the size and hash of every other file in the package.
    const std::string runtime_args = "--pack " + std::string(kRuntimeDir) + "/" + std::string(kPackFile) + " --start-scene " + guid +
                                     " --backend " + backend + " --profile " + profile;
    const auto files = package_files(core::to_native_path(staging));
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& relative : files) {
        auto bytes_of = core::read_file(to_core(core::to_native_path(staging) / relative));
        if (!bytes_of.ok()) {
            return core::failure("could not read " + utf8(relative) + " for the manifest");
        }
        entries.push_back({{"path", relative.generic_string()},
                           {"size", bytes_of.value().size()},
                           {"sha256", assets::ContentHash::of(bytes_of.value()).to_hex()}});
    }
    report.files = files.size();
    const nlohmann::json manifest = {
        {"format", std::string(kPackageFormat)},
        {"version", kPackageVersion},
        {"name", name},
        {"platform", std::string(kPlatform)},
        {"executable", kRuntimeName},
        {"launcher", std::string(kLauncher)},
        {"start_scene", guid},
        {"backend", backend},
        {"profile", profile},
        {"arguments", runtime_args},
        {"packs", nlohmann::json::array({std::string(kRuntimeDir) + "/" + std::string(kPackFile)})},
        {"files", entries},
    };
    if (auto written = core::write_file_atomic(child(staging, kPackageManifest), manifest.dump(2) + "\n"); !written.ok()) {
        return core::failure("could not write " + std::string(kPackageManifest) + ": " + written.error().path);
    }
    report.log.push_back("assembled " + std::to_string(report.files) + " files");
    if (cancelled()) {
        return core::failure(std::string(kCancelled));
    }

    // Verify: copy the staged package into an empty folder and start it there. The start scene must report itself.
    if (options.verify) {
        set_stage(BuildStage::Verify);
        const Path clean = child(build_dir, "verify");
        static_cast<void>(core::remove_all(clean));
        ScopedRemove clean_guard(clean);
        if (auto made = core::create_directories(clean); !made.ok()) {
            return core::failure("could not create the verify folder: " + clean.str());
        }
        fs::copy(core::to_native_path(staging), core::to_native_path(clean), fs::copy_options::recursive, ec);
        if (ec) {
            return core::failure("could not copy the package for the launch check: " + ec.message());
        }
        const Path log = child(build_dir, "verify.log");
        const std::string arguments = runtime_args;
        const int status = run_runtime(core::to_native_path(clean), arguments, core::to_native_path(log));
        auto output_text = core::read_text_file(log);
        const std::string text = output_text.ok() ? output_text.value() : std::string();
        if (status != 0 || text.find(std::string(kStartedText) + guid) == std::string::npos) {
            return core::failure("the package did not launch from a clean folder (exit " + std::to_string(status) + "): " + text);
        }
        report.verified = true;
        report.log.push_back("launched from a clean folder: " + std::string(kStartedText) + guid);
    }
    if (cancelled()) {
        return core::failure(std::string(kCancelled));
    }

    // Publish: move the staged package into place, keeping the previous package until the move succeeds.
    const Path previous = child(build_dir, "previous");
    static_cast<void>(core::remove_all(previous));
    const bool replacing = core::path_exists(target);
    if (replacing) {
        std::error_code move_ec;
        fs::rename(core::to_native_path(target), core::to_native_path(previous), move_ec);
        if (move_ec) {
            return core::failure("could not replace the existing package: " + move_ec.message());
        }
    }
    std::error_code rename_ec;
    fs::rename(core::to_native_path(staging), core::to_native_path(target), rename_ec);
    if (rename_ec) {
        if (replacing) {
            std::error_code restore_ec;
            fs::rename(core::to_native_path(previous), core::to_native_path(target), restore_ec);
        }
        return core::failure("could not publish the package: " + rename_ec.message());
    }
    staging_guard.keep();
    static_cast<void>(core::remove_all(previous));
    report.log.push_back("published " + target.str());
    set_stage(BuildStage::Done);
    return core::success(std::move(report));
}

GameBuilder::~GameBuilder() {
    cancel();
    if (future_.valid()) {
        future_.wait();
    }
}

core::Result<core::Unit, std::string> GameBuilder::start(GameBuildOptions options) {
    if (future_.valid()) {
        return core::failure(std::string("a build is already running"));
    }
    progress_.cancel.store(false);
    progress_.stage.store(static_cast<std::uint8_t>(BuildStage::Idle));
    progress_.done.store(0);
    progress_.total.store(0);
    future_ = std::async(std::launch::async, [this, options = std::move(options)]() {
        const auto importers = converter::ImporterRegistry::builtin();
        return build_game(options, progress_, importers);
    });
    return core::success();
}

void GameBuilder::cancel() noexcept {
    progress_.cancel.store(true);
}

std::optional<core::Result<GameBuildReport, std::string>> GameBuilder::poll() {
    if (!future_.valid() || future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return std::nullopt;
    }
    return future_.get();
}

}  // namespace engine::editor
