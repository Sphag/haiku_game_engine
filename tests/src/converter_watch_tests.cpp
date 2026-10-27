// Source watching (US-025): debouncing, snapshots, when a pass runs, and the OS watch. The unit cases script the change source,
// so every pass is decided by the test. The integration cases use the OS watch on a scratch project and wait for real
// notifications. Each test writes its project into a scratch directory that it removes.

#include "asset_converter/converter.hpp"
#include "asset_converter/watch.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using engine::converter::AssetOutcome;
using engine::converter::AssetStatus;
using engine::converter::ChangeSource;
using engine::converter::Debouncer;
using engine::converter::ErrorCode;
using engine::converter::ImporterRegistry;
using engine::converter::Options;
using engine::converter::Report;
using engine::converter::WatchEvents;
using engine::converter::WatchOptions;
using engine::converter::WatchSession;
using engine::core::Guid;
using engine::core::Path;

using namespace std::chrono_literals;

namespace {

std::string utf8(const fs::path& value) {
    const auto text = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

Path path_of(const fs::path& value) {
    return Path::from_utf8(utf8(value)).value();
}

// A scratch directory, removed when the test ends.
class ScratchDir {
public:
    ScratchDir() : path_(fs::temp_directory_path() / ("engine_watch_" + Guid::generate().to_string())) {
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

void write_text(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_bytes(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::ostringstream bytes;
    bytes << stream.rdbuf();
    return bytes.str();
}

// Writes the sidecar of `source` (project-relative) for a blob or json asset with the given dependencies. Returns its GUID.
Guid write_sidecar(const fs::path& project, const std::string& source, const std::string& importer,
                   const std::vector<Guid>& dependencies = {}) {
    const Guid guid = Guid::generate();
    std::string deps;
    for (const Guid& dependency : dependencies) {
        deps += (deps.empty() ? "" : ", ") + std::string("\"") + dependency.to_string() + "\"";
    }
    write_text(project / (source + ".meta"),
               "{\"format\": \"engine.asset_meta\", \"version\": 1, \"guid\": \"" + guid.to_string() +
                   "\", \"importer\": {\"type\": \"" + importer + "\", \"settings\": {}}, \"dependencies\": [" + deps + "]}");
    return guid;
}

// Notifies about one path.
WatchEvents changed(std::string path) {
    return WatchEvents{{std::move(path)}, false};
}

// A change source the test fills. Each poll returns the next queued batch, or nothing when the queue is empty.
class ScriptedSource final : public ChangeSource {
public:
    void push(WatchEvents events) {
        queued_.push_back(std::move(events));
    }

    WatchEvents poll(std::chrono::milliseconds timeout) override {
        if (queued_.empty()) {
            std::this_thread::sleep_for(timeout);  // a real wait, so a test loop does not spin
            return {};
        }
        WatchEvents next = std::move(queued_.front());
        queued_.pop_front();
        return next;
    }

private:
    std::deque<WatchEvents> queued_;
};

// The outcome of one GUID in a report, when the report has it.
std::optional<AssetOutcome> outcome_of(const Report& report, const Guid& guid) {
    for (const AssetOutcome& outcome : report.assets) {
        if (outcome.guid == guid.to_string()) {
            return outcome;
        }
    }
    return std::nullopt;
}

// A project with an output directory. The output may sit inside the project, which is the case the loop guard covers.
struct Fixture {
    explicit Fixture(bool output_inside_project)
        : project(scratch.path() / "project"), output(output_inside_project ? project / "out" : scratch.path() / "out") {
        fs::create_directories(project);
    }

    Options options() const {
        return Options{path_of(project), std::optional<Path>(path_of(output)), 1};
    }

    ScratchDir scratch;
    fs::path project;
    fs::path output;
};

// Steps the session for `span`, collecting every pass that ran.
std::vector<Report> run_for(WatchSession& session, std::chrono::milliseconds span) {
    std::vector<Report> passes;
    const auto deadline = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < deadline) {
        auto next = session.step(20ms);
        REQUIRE(next.ok());
        if (next.value()) {
            passes.push_back(*next.value());
        }
    }
    return passes;
}

// Fast windows for the tests: a pass follows 100 ms of quiet, and a steady stream waits at most 500 ms.
constexpr WatchOptions kFastWatch{100ms, 500ms};

}  // namespace

TEST_CASE("Debouncer: a burst becomes one pass once the quiet window has passed", "[unit]") {
    Debouncer debouncer(WatchOptions{100ms, 1000ms});
    const auto t0 = Debouncer::Clock::now();

    CHECK_FALSE(debouncer.pending());
    CHECK_FALSE(debouncer.until_due(t0).has_value());

    debouncer.note(t0);
    CHECK(debouncer.pending());
    CHECK_FALSE(debouncer.due(t0 + 99ms));
    CHECK(debouncer.due(t0 + 100ms));

    debouncer.note(t0 + 90ms);
    CHECK_FALSE(debouncer.due(t0 + 150ms));
    CHECK(debouncer.due(t0 + 190ms));

    const auto remaining = debouncer.until_due(t0 + 150ms);
    REQUIRE(remaining.has_value());
    CHECK(*remaining == 40ms);

    debouncer.clear();
    CHECK_FALSE(debouncer.pending());
    CHECK_FALSE(debouncer.due(t0 + 1000ms));
}

TEST_CASE("Debouncer: a steady stream of changes still produces a pass at the maximum wait", "[unit]") {
    Debouncer debouncer(WatchOptions{100ms, 300ms});
    const auto t0 = Debouncer::Clock::now();
    for (auto offset : {0ms, 50ms, 100ms, 150ms, 200ms, 250ms}) {
        debouncer.note(t0 + offset);
    }

    CHECK_FALSE(debouncer.due(t0 + 299ms));
    CHECK(debouncer.due(t0 + 300ms));
}

TEST_CASE("Snapshot: content hashes change with the bytes, and a touch without a change leaves the snapshot equal", "[unit]") {
    ScratchDir scratch;
    const fs::path project = scratch.path() / "project";
    write_text(project / "a.txt", "one");
    write_text(project / "sub" / "b.txt", "two");

    auto first = engine::converter::take_snapshot(path_of(project), std::nullopt);
    REQUIRE(first.ok());
    CHECK(first.value().size() == 2);
    CHECK(first.value().count("a.txt") == 1);
    CHECK(first.value().count("sub/b.txt") == 1);

    write_text(project / "a.txt", "one");
    auto touched = engine::converter::take_snapshot(path_of(project), std::nullopt);
    REQUIRE(touched.ok());
    CHECK(touched.value() == first.value());

    write_text(project / "a.txt", "changed");
    auto changed = engine::converter::take_snapshot(path_of(project), std::nullopt);
    REQUIRE(changed.ok());
    CHECK_FALSE(changed.value() == first.value());
}

TEST_CASE("Snapshot: the skipped directory is not listed", "[unit]") {
    ScratchDir scratch;
    const fs::path project = scratch.path() / "project";
    write_text(project / "a.txt", "source");
    write_text(project / "out" / "a.cooked", "output");

    auto snapshot = engine::converter::take_snapshot(path_of(project), path_of(project / "out"));
    REQUIRE(snapshot.ok());
    CHECK(snapshot.value().size() == 1);
    CHECK(snapshot.value().count("a.txt") == 1);
}

TEST_CASE("Watch session: notifications without a content change run no pass", "[unit]") {
    Fixture fixture(false);
    const Guid guid = write_sidecar(fixture.project, "a.bin", "blob");
    write_text(fixture.project / "a.bin", "payload");
    ScriptedSource source;
    WatchSession session(fixture.options(), ImporterRegistry::builtin(), source, kFastWatch);
    REQUIRE(session.start().ok());

    source.push(changed("a.bin"));
    source.push(changed("a.bin"));
    CHECK(run_for(session, 200ms).empty());
    CHECK(fs::exists(fixture.output / (guid.to_string() + ".cooked")));
}

TEST_CASE("Watch session: a changed source runs one pass that converts it", "[unit]") {
    Fixture fixture(false);
    const Guid guid = write_sidecar(fixture.project, "a.bin", "blob");
    write_text(fixture.project / "a.bin", "first");
    ScriptedSource source;
    WatchSession session(fixture.options(), ImporterRegistry::builtin(), source, kFastWatch);
    auto first = session.start();
    REQUIRE(first.ok());
    REQUIRE(outcome_of(first.value(), guid).has_value());
    CHECK(outcome_of(first.value(), guid)->status == AssetStatus::Cooked);

    write_text(fixture.project / "a.bin", "second");
    source.push(changed("a.bin"));
    const auto passes = run_for(session, 300ms);
    REQUIRE(passes.size() == 1);
    const auto outcome = outcome_of(passes.front(), guid);
    REQUIRE(outcome.has_value());
    CHECK(outcome->status == AssetStatus::Cooked);
    CHECK(passes.front().ok());
}

TEST_CASE("Watch session: changes inside the output directory never start a pass", "[unit]") {
    Fixture fixture(true);
    write_sidecar(fixture.project, "a.bin", "blob");
    write_text(fixture.project / "a.bin", "payload");
    ScriptedSource source;
    WatchSession session(fixture.options(), ImporterRegistry::builtin(), source, kFastWatch);
    REQUIRE(session.start().ok());

    // The converter's own writes arrive as notifications under out/. Even with a real content change in the output, nothing
    // reads it, so no pass runs.
    write_text(fixture.output / "stray.cooked", "written by the converter");
    source.push(changed("out/stray.cooked"));
    source.push(changed("out/stray.cooked"));
    CHECK(run_for(session, 200ms).empty());
}

TEST_CASE("Watch session: a renamed source is converted under its new path with the same GUID", "[unit]") {
    Fixture fixture(false);
    const Guid guid = write_sidecar(fixture.project, "old.bin", "blob");
    write_text(fixture.project / "old.bin", "payload");
    ScriptedSource source;
    WatchSession session(fixture.options(), ImporterRegistry::builtin(), source, kFastWatch);
    REQUIRE(session.start().ok());

    fs::rename(fixture.project / "old.bin", fixture.project / "new.bin");
    fs::rename(fixture.project / "old.bin.meta", fixture.project / "new.bin.meta");
    source.push(WatchEvents{{"old.bin", "new.bin"}, false});
    const auto passes = run_for(session, 300ms);
    REQUIRE(passes.size() == 1);
    const auto outcome = outcome_of(passes.front(), guid);
    REQUIRE(outcome.has_value());
    CHECK(outcome->source == "new.bin");
    CHECK(outcome->status != AssetStatus::Failed);
}

TEST_CASE("Watch session: deleting a dependency stops its dependent from converting and keeps the previous output", "[unit]") {
    Fixture fixture(false);
    const Guid base = write_sidecar(fixture.project, "base.bin", "blob");
    const Guid dependent = write_sidecar(fixture.project, "dep.bin", "blob", {base});
    write_text(fixture.project / "base.bin", "base");
    write_text(fixture.project / "dep.bin", "dependent");
    ScriptedSource source;
    WatchSession session(fixture.options(), ImporterRegistry::builtin(), source, kFastWatch);
    auto first = session.start();
    REQUIRE(first.ok());
    REQUIRE(first.value().ok());
    const fs::path cooked = fixture.output / (dependent.to_string() + ".cooked");
    const std::string before = read_bytes(cooked);
    REQUIRE_FALSE(before.empty());

    fs::remove(fixture.project / "base.bin");
    fs::remove(fixture.project / "base.bin.meta");
    source.push(changed("base.bin"));
    const auto passes = run_for(session, 300ms);
    REQUIRE(passes.size() == 1);
    CHECK_FALSE(passes.front().ok());
    const auto outcome = outcome_of(passes.front(), dependent);
    REQUIRE(outcome.has_value());
    // A missing dependency is the dependent's own error, so it fails. What matters is that it is not converted again.
    CHECK((outcome->status == AssetStatus::Failed || outcome->status == AssetStatus::Blocked));

    bool reported = false;
    for (const auto& diagnostic : passes.front().diagnostics) {
        reported = reported || diagnostic.code == ErrorCode::MissingDependency || diagnostic.code == ErrorCode::DependencyBlocked;
    }
    CHECK(reported);
    CHECK(read_bytes(cooked) == before);
}

TEST_CASE("Watch session: a broken source keeps the previous working output and reports the failure", "[unit]") {
    Fixture fixture(false);
    const Guid guid = write_sidecar(fixture.project, "config.json", "json");
    write_text(fixture.project / "config.json", "{\"speed\": 2}");
    ScriptedSource source;
    WatchSession session(fixture.options(), ImporterRegistry::builtin(), source, kFastWatch);
    REQUIRE(session.start().ok());
    const fs::path cooked = fixture.output / (guid.to_string() + ".cooked");
    const std::string working = read_bytes(cooked);

    write_text(fixture.project / "config.json", "{\"speed\": ");
    source.push(changed("config.json"));
    const auto passes = run_for(session, 300ms);
    REQUIRE(passes.size() == 1);
    CHECK_FALSE(passes.front().ok());
    const auto outcome = outcome_of(passes.front(), guid);
    REQUIRE(outcome.has_value());
    CHECK(outcome->status == AssetStatus::Failed);
    bool reported = false;
    for (const auto& diagnostic : passes.front().diagnostics) {
        reported = reported || diagnostic.code == ErrorCode::ImportFailed;
    }
    CHECK(reported);
    CHECK(read_bytes(cooked) == working);

    write_text(fixture.project / "config.json", "{\"speed\": 3}");
    source.push(changed("config.json"));
    const auto repaired = run_for(session, 300ms);
    REQUIRE(repaired.size() == 1);
    CHECK(repaired.front().ok());
}

TEST_CASE("Watch session: starting without an output directory is a usage error", "[unit]") {
    Fixture fixture(false);
    write_sidecar(fixture.project, "a.bin", "blob");
    write_text(fixture.project / "a.bin", "payload");
    ScriptedSource source;
    Options options = fixture.options();
    options.output.reset();
    WatchSession session(options, ImporterRegistry::builtin(), source, kFastWatch);
    CHECK_FALSE(session.start().ok());
}

TEST_CASE("Watch: the OS watch converts an edited source and its own output does not start another pass", "[integration]") {
    Fixture fixture(true);
    const Guid guid = write_sidecar(fixture.project, "a.bin", "blob");
    write_text(fixture.project / "a.bin", "first");

    auto source = engine::converter::open_directory_watch(path_of(fixture.project));
    REQUIRE(source.ok());
    WatchSession session(fixture.options(), ImporterRegistry::builtin(), *source.value(), kFastWatch);
    REQUIRE(session.start().ok());

    write_text(fixture.project / "a.bin", "second");
    const auto passes = run_for(session, 1500ms);
    REQUIRE(passes.size() == 1);
    const auto outcome = outcome_of(passes.front(), guid);
    REQUIRE(outcome.has_value());
    CHECK(outcome->status == AssetStatus::Cooked);

    // The pass wrote into out/, which is inside the project. Those writes must not start another pass.
    CHECK(run_for(session, 800ms).empty());
}

TEST_CASE("Watch: the OS watch converts a source added after the watch started", "[integration]") {
    Fixture fixture(false);
    write_sidecar(fixture.project, "seed.bin", "blob");
    write_text(fixture.project / "seed.bin", "seed");

    auto source = engine::converter::open_directory_watch(path_of(fixture.project));
    REQUIRE(source.ok());
    WatchSession session(fixture.options(), ImporterRegistry::builtin(), *source.value(), kFastWatch);
    REQUIRE(session.start().ok());

    // The directory is new, so this also needs the watch to cover directories created later.
    const Guid added = write_sidecar(fixture.project / "nested", "late.bin", "blob");
    write_text(fixture.project / "nested" / "late.bin", "late");
    std::vector<Report> passes;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (passes.empty() && std::chrono::steady_clock::now() < deadline) {
        auto collected = run_for(session, 100ms);
        passes.insert(passes.end(), collected.begin(), collected.end());
    }
    bool converted = false;
    for (const Report& pass : passes) {
        const auto outcome = outcome_of(pass, added);
        converted = converted || (outcome.has_value() && outcome->status == AssetStatus::Cooked);
    }
    CHECK(converted);
}

TEST_CASE("Watch: the OS watch sees a rename and converts the source under its new path", "[integration]") {
    Fixture fixture(false);
    const Guid guid = write_sidecar(fixture.project, "before.bin", "blob");
    write_text(fixture.project / "before.bin", "payload");

    auto source = engine::converter::open_directory_watch(path_of(fixture.project));
    REQUIRE(source.ok());
    WatchSession session(fixture.options(), ImporterRegistry::builtin(), *source.value(), kFastWatch);
    REQUIRE(session.start().ok());

    fs::rename(fixture.project / "before.bin", fixture.project / "after.bin");
    fs::rename(fixture.project / "before.bin.meta", fixture.project / "after.bin.meta");
    std::vector<Report> passes;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (passes.empty() && std::chrono::steady_clock::now() < deadline) {
        auto collected = run_for(session, 100ms);
        passes.insert(passes.end(), collected.begin(), collected.end());
    }
    bool renamed = false;
    for (const Report& pass : passes) {
        const auto outcome = outcome_of(pass, guid);
        renamed = renamed || (outcome.has_value() && outcome->source == "after.bin");
    }
    CHECK(renamed);
}
