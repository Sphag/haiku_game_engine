// Runtime asset handles (US-024): asynchronous loads of cooked files. A test decides when a decode may finish (a gate), so the
// states it checks are the ones it means, not whatever a worker thread happened to reach. Files are written into a scratch
// directory that each test removes.

#include "asset_converter/converter.hpp"
#include "asset_converter/importer.hpp"
#include "engine/assets/asset_handle.hpp"
#include "engine/assets/cooked_container.hpp"
#include "engine/assets/texture_format.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/jobs.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

using engine::assets::AssetHandle;
using engine::assets::AssetManager;
using engine::assets::fallback_texture;
using engine::assets::LoadState;
using engine::assets::read_texture_payload;
using engine::assets::TextureEncoding;
using engine::assets::TexturePayload;
using engine::assets::write_cooked_container;
using engine::core::Guid;
using engine::core::JobSystem;
using engine::core::Path;
using engine::core::Result;

namespace {

using TextHandle = AssetHandle<std::string>;
using TextManager = AssetManager<std::string>;

std::string utf8(const fs::path& value) {
    const auto text = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

Path path_of(const fs::path& value) {
    return Path::from_utf8(utf8(value)).value();
}

std::string read_bytes(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::ostringstream bytes;
    bytes << stream.rdbuf();
    return bytes.str();
}

// A scratch directory, removed when the test ends.
class ScratchDir {
public:
    ScratchDir() : path_(fs::temp_directory_path() / ("engine_handles_" + Guid::generate().to_string())) {
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

// Writes the file a cooked load reads. The bytes are whatever the test needs: a valid container, or a broken one.
void write_cooked(const fs::path& root, const Guid& guid, const std::string& bytes) {
    std::ofstream out(root / (guid.to_string() + ".cooked"), std::ios::binary);
    out << bytes;
}

// Holds decodes at the gate until the test releases them. While it is held, a decode cannot finish.
class Gate {
public:
    void hold() {
        const std::lock_guard lock(mutex_);
        held_ = true;
    }

    void release() {
        {
            const std::lock_guard lock(mutex_);
            held_ = false;
        }
        released_.notify_all();
    }

    void pass() {
        std::unique_lock lock(mutex_);
        released_.wait(lock, [this] { return !held_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable released_;
    bool held_ = false;
};

// What a test observes about the decoder: how many decodes started, and the gate they pass through.
struct Probe {
    std::atomic<int> decodes{0};
    Gate gate;
};

// Polls until the condition holds. A test that needs a decode to be in flight uses it, with a limit so a broken
// implementation fails instead of hanging.
bool wait_until(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Decodes a text payload. "BAD" is a payload the decoder rejects, and "THROW" makes it throw.
Result<std::string, std::string> decode_text(std::string_view payload) {
    if (payload == "THROW") {
        throw std::runtime_error("decoder exploded");
    }
    if (payload == "BAD") {
        return engine::core::failure(std::string("bad payload"));
    }
    return engine::core::success(std::string(payload));
}

TextManager make_text_manager(JobSystem& jobs, const fs::path& root, Probe& probe) {
    return TextManager(
        jobs, path_of(root),
        [&probe](std::string_view payload) {
            probe.decodes.fetch_add(1);
            probe.gate.pass();
            return decode_text(payload);
        },
        std::make_shared<const std::string>("fallback"));
}

}  // namespace

TEST_CASE("Asset handle: an empty handle is failed, has no payload, and has no error to show", "[unit]") {
    const TextHandle empty;

    CHECK_FALSE(empty.valid());
    CHECK(empty.state() == LoadState::Failed);
    CHECK_FALSE(empty.ready());
    CHECK(empty.get() == nullptr);
    CHECK(empty.error() == "the handle is empty");
    CHECK(empty.guid() == Guid{});
}

TEST_CASE("Asset handle: a request is loading until its decode finishes, then ready with the payload", "[integration]") {
    ScratchDir scratch;
    const Guid id = Guid::generate();
    write_cooked(scratch.path(), id, write_cooked_container("hello"));
    JobSystem jobs(2);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);

    probe.gate.hold();
    auto handle = manager.request(id);
    REQUIRE(wait_until([&probe] { return probe.decodes.load() == 1; }));
    CHECK(handle.state() == LoadState::Loading);
    CHECK_FALSE(handle.ready());
    CHECK(*handle.get() == "fallback");
    CHECK(handle.error().empty());

    probe.gate.release();
    jobs.wait_idle();
    CHECK(handle.state() == LoadState::Ready);
    REQUIRE(handle.get() != nullptr);
    CHECK(*handle.get() == "hello");
    CHECK(handle.guid() == id);
}

TEST_CASE("Asset handle: a missing cooked file fails with the fallback, and a failed load is not retried while held", "[integration]") {
    ScratchDir scratch;
    JobSystem jobs(2);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);
    const Guid id = Guid::generate();

    auto handle = manager.request(id);
    jobs.wait_idle();
    CHECK(handle.state() == LoadState::Failed);
    CHECK(handle.error().find("NotFound") != std::string::npos);
    CHECK(*handle.get() == "fallback");

    // The file appears now. The handle is still held, so the failed entry is shared and nothing is read again.
    write_cooked(scratch.path(), id, write_cooked_container("late"));
    auto again = manager.request(id);
    jobs.wait_idle();
    CHECK(again.state() == LoadState::Failed);
    CHECK(manager.live_entries() == 1);

    // Releasing every handle lets the next request start over, and this time it reads the file.
    handle = TextHandle{};
    again = TextHandle{};
    CHECK(manager.live_entries() == 0);
    auto reloaded = manager.request(id);
    jobs.wait_idle();
    CHECK(reloaded.state() == LoadState::Ready);
    CHECK(*reloaded.get() == "late");
}

TEST_CASE("Asset handle: duplicate requests share one load and one decode", "[integration]") {
    ScratchDir scratch;
    const Guid id = Guid::generate();
    write_cooked(scratch.path(), id, write_cooked_container("shared"));
    JobSystem jobs(2);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);

    probe.gate.hold();
    auto first = manager.request(id);
    REQUIRE(wait_until([&probe] { return probe.decodes.load() == 1; }));
    auto second = manager.request(id);
    CHECK(manager.live_entries() == 1);

    probe.gate.release();
    jobs.wait_idle();
    CHECK(probe.decodes.load() == 1);
    REQUIRE(first.ready());
    REQUIRE(second.ready());
    CHECK(first.get().get() == second.get().get());
    CHECK(*second.get() == "shared");
}

TEST_CASE("Asset handle: releasing the last handle unloads the asset, and the next request reloads it", "[integration]") {
    ScratchDir scratch;
    const Guid id = Guid::generate();
    write_cooked(scratch.path(), id, write_cooked_container("reload"));
    JobSystem jobs(2);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);

    auto handle = manager.request(id);
    jobs.wait_idle();
    REQUIRE(handle.ready());
    auto copy = handle;

    handle = TextHandle{};
    CHECK(manager.live_entries() == 1);
    CHECK(*copy.get() == "reload");

    copy = TextHandle{};
    CHECK(manager.live_entries() == 0);

    auto reloaded = manager.request(id);
    jobs.wait_idle();
    CHECK(probe.decodes.load() == 2);
    CHECK(reloaded.state() == LoadState::Ready);
    CHECK(*reloaded.get() == "reload");
}

TEST_CASE("Asset handle: format errors fail the handle and never publish a payload", "[integration]") {
    ScratchDir scratch;
    JobSystem jobs(2);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);

    SECTION("bytes that are not a cooked container") {
        const Guid id = Guid::generate();
        write_cooked(scratch.path(), id, "this is not a container at all");
        auto handle = manager.request(id);
        jobs.wait_idle();
        CHECK(handle.state() == LoadState::Failed);
        CHECK(handle.error().find("not a cooked container") != std::string::npos);
        CHECK(*handle.get() == "fallback");
        CHECK(probe.decodes.load() == 0);
    }
    SECTION("a container cut short") {
        const Guid id = Guid::generate();
        write_cooked(scratch.path(), id, write_cooked_container("hello").substr(0, 18));
        auto handle = manager.request(id);
        jobs.wait_idle();
        CHECK(handle.state() == LoadState::Failed);
        CHECK(handle.error().find("does not match") != std::string::npos);
        CHECK(*handle.get() == "fallback");
        CHECK(probe.decodes.load() == 0);
    }
    SECTION("a valid container whose payload the decoder rejects") {
        const Guid id = Guid::generate();
        write_cooked(scratch.path(), id, write_cooked_container("BAD"));
        auto handle = manager.request(id);
        jobs.wait_idle();
        CHECK(handle.state() == LoadState::Failed);
        CHECK(handle.error().find("bad payload") != std::string::npos);
        CHECK(handle.error().find(id.to_string()) != std::string::npos);
        CHECK(*handle.get() == "fallback");
        CHECK(probe.decodes.load() == 1);
    }
}

TEST_CASE("Asset handle: a decoder that throws fails the handle with its message", "[integration]") {
    ScratchDir scratch;
    const Guid id = Guid::generate();
    write_cooked(scratch.path(), id, write_cooked_container("THROW"));
    JobSystem jobs(2);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);

    auto handle = manager.request(id);
    jobs.wait_idle();
    CHECK(handle.state() == LoadState::Failed);
    CHECK(handle.error().find("decoder exploded") != std::string::npos);
    CHECK(*handle.get() == "fallback");
}

TEST_CASE("Asset handle: a consumer destroyed mid-decode cancels its load and publishes nothing", "[integration]") {
    ScratchDir scratch;
    const Guid id = Guid::generate();
    write_cooked(scratch.path(), id, write_cooked_container("late"));
    JobSystem jobs(2);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);

    probe.gate.hold();
    auto handle = manager.request(id);
    REQUIRE(wait_until([&probe] { return probe.decodes.load() == 1; }));

    // The last handle goes while its decode is still running.
    handle = TextHandle{};
    CHECK(manager.live_entries() == 0);

    probe.gate.release();
    jobs.wait_idle();
    CHECK(manager.live_entries() == 0);

    // The finished job left nothing behind, so a new request loads the asset normally.
    auto fresh = manager.request(id);
    jobs.wait_idle();
    CHECK(fresh.state() == LoadState::Ready);
    CHECK(*fresh.get() == "late");
    CHECK(probe.decodes.load() == 2);
}

TEST_CASE("Asset handle: a consumer destroyed while its load is still queued never starts the decode", "[integration]") {
    ScratchDir scratch;
    const Guid running_id = Guid::generate();
    const Guid queued_id = Guid::generate();
    write_cooked(scratch.path(), running_id, write_cooked_container("first"));
    write_cooked(scratch.path(), queued_id, write_cooked_container("second"));
    JobSystem jobs(1);  // one worker, so the second load waits behind the first
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);

    probe.gate.hold();
    auto running = manager.request(running_id);
    REQUIRE(wait_until([&probe] { return probe.decodes.load() == 1; }));
    {
        auto queued = manager.request(queued_id);
        CHECK(queued.state() == LoadState::Loading);
    }
    CHECK(manager.live_entries() == 1);

    probe.gate.release();
    jobs.wait_idle();
    CHECK(probe.decodes.load() == 1);
    CHECK(running.state() == LoadState::Ready);
}

TEST_CASE("Asset handle: closing the world cancels pending loads and refuses later requests", "[integration]") {
    ScratchDir scratch;
    const Guid running_id = Guid::generate();
    const Guid pending_id = Guid::generate();
    const Guid refused_id = Guid::generate();
    write_cooked(scratch.path(), running_id, write_cooked_container("running"));
    write_cooked(scratch.path(), pending_id, write_cooked_container("pending"));
    write_cooked(scratch.path(), refused_id, write_cooked_container("refused"));
    JobSystem jobs(1);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);

    probe.gate.hold();
    auto running = manager.request(running_id);
    REQUIRE(wait_until([&probe] { return probe.decodes.load() == 1; }));
    auto pending = manager.request(pending_id);

    manager.close();
    CHECK(manager.closed());
    CHECK(running.state() == LoadState::Failed);
    CHECK(running.error().find("closed") != std::string::npos);
    CHECK(pending.state() == LoadState::Failed);

    auto refused = manager.request(refused_id);
    CHECK(refused.state() == LoadState::Failed);
    CHECK(refused.error().find("closed") != std::string::npos);

    // The running decode finishes after closure and must not publish. The pending load never starts.
    probe.gate.release();
    jobs.wait_idle();
    CHECK(running.state() == LoadState::Failed);
    CHECK(*running.get() == "fallback");
    CHECK(*pending.get() == "fallback");
    CHECK(probe.decodes.load() == 1);
}

TEST_CASE("Asset handle: closing the world keeps the payloads that already loaded", "[integration]") {
    ScratchDir scratch;
    const Guid id = Guid::generate();
    write_cooked(scratch.path(), id, write_cooked_container("kept"));
    JobSystem jobs(2);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);

    auto loaded = manager.request(id);
    jobs.wait_idle();
    REQUIRE(loaded.ready());

    manager.close();
    CHECK(loaded.ready());
    CHECK(*loaded.get() == "kept");
}

TEST_CASE("Asset handle: a texture cooked by the converter loads through a handle", "[integration]") {
    const fs::path root = fs::temp_directory_path() / ("engine_handles_texture_" + Guid::generate().to_string());
    const fs::path project = root / "project";
    const fs::path output = root / "out";
    fs::create_directories(project / "textures");
    const std::string guid_text = "7d3e9a10-2b4c-4e6f-8a1b-0c2d3e4f5a61";
    {
        std::ofstream(project / "textures" / "gradient.png", std::ios::binary)
            << read_bytes(fs::path(std::string(ENGINE_TEST_FIXTURE_DIR)) / "textures" / "gradient_4x4.png");
        std::ofstream meta(project / "textures" / "gradient.png.meta", std::ios::binary);
        meta << R"({
  "format": "engine.asset_meta",
  "version": 1,
  "guid": ")" << guid_text << R"(",
  "importer": {
    "type": "texture",
    "settings": {"usage": "color"}
  },
  "dependencies": []
})";
    }
    auto converted = engine::converter::run(engine::converter::Command::Cook,
                                            engine::converter::Options{path_of(project), path_of(output), 0},
                                            engine::converter::ImporterRegistry::builtin());
    INFO("convert failed: " << (converted.ok() ? "" : converted.error()));
    REQUIRE(converted.ok());
    REQUIRE(converted.value().ok());

    JobSystem jobs(2);
    AssetManager<TexturePayload> textures(jobs, path_of(output), read_texture_payload,
                                          std::make_shared<const TexturePayload>(fallback_texture()));
    auto id = Guid::parse(guid_text);
    REQUIRE(id.ok());
    auto handle = textures.request(id.value());
    jobs.wait_idle();

    INFO("texture load failed: " << handle.error());
    REQUIRE(handle.ready());
    const auto texture = handle.get();
    CHECK(texture->encoding == TextureEncoding::Bc7);
    CHECK(texture->srgb);
    CHECK(texture->mips.size() == 3);
    CHECK(texture->mips.front().width == 4);

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

TEST_CASE("Asset reload: a rebuilt file is staged while the old payload stays visible until commit", "[integration]") {
    ScratchDir scratch;
    const Guid id = Guid::generate();
    write_cooked(scratch.path(), id, write_cooked_container("first"));
    JobSystem jobs(2);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);
    auto handle = manager.request(id);
    jobs.wait_idle();
    REQUIRE(handle.ready());

    write_cooked(scratch.path(), id, write_cooked_container("second"));
    probe.gate.hold();
    REQUIRE(manager.reload(handle));
    CHECK(handle.reloading());
    REQUIRE(wait_until([&probe] { return probe.decodes.load() == 2; }));
    CHECK(*handle.get() == "first");
    CHECK_FALSE(handle.commit_reload());

    probe.gate.release();
    jobs.wait_idle();
    CHECK(handle.reloading() == false);
    CHECK(*handle.get() == "first");
    CHECK(handle.reload_error().empty());

    CHECK(manager.apply_reloads() == 1);
    CHECK(*handle.get() == "second");
    CHECK(manager.apply_reloads() == 0);
}

TEST_CASE("Asset reload: a failed rebuild keeps the working payload and reports why", "[integration]") {
    ScratchDir scratch;
    const Guid id = Guid::generate();
    write_cooked(scratch.path(), id, write_cooked_container("working"));
    JobSystem jobs(2);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);
    auto handle = manager.request(id);
    jobs.wait_idle();
    REQUIRE(handle.ready());

    write_cooked(scratch.path(), id, write_cooked_container("BAD"));
    REQUIRE(manager.reload(handle));
    jobs.wait_idle();
    CHECK(handle.state() == LoadState::Ready);
    CHECK(*handle.get() == "working");
    CHECK_FALSE(handle.reloading());
    CHECK(handle.reload_error().find("bad payload") != std::string::npos);
    CHECK_FALSE(handle.commit_reload());
    CHECK(manager.apply_reloads() == 0);
    CHECK(*handle.get() == "working");

    write_cooked(scratch.path(), id, write_cooked_container("repaired"));
    REQUIRE(manager.reload(handle));
    jobs.wait_idle();
    CHECK(handle.reload_error().empty());
    CHECK(manager.apply_reloads() == 1);
    CHECK(*handle.get() == "repaired");
}

TEST_CASE("Asset reload: a missing file fails the reload and a second reload is refused while one is in flight", "[integration]") {
    ScratchDir scratch;
    const Guid id = Guid::generate();
    write_cooked(scratch.path(), id, write_cooked_container("kept"));
    JobSystem jobs(2);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);
    auto handle = manager.request(id);
    jobs.wait_idle();
    REQUIRE(handle.ready());

    CHECK_FALSE(manager.reload(AssetHandle<std::string>{}));

    fs::remove(scratch.path() / (id.to_string() + ".cooked"));
    probe.gate.hold();
    REQUIRE(manager.reload(handle));
    CHECK_FALSE(manager.reload(handle));
    REQUIRE(wait_until([&probe] { return probe.decodes.load() == 1; }));
    probe.gate.release();
    jobs.wait_idle();
    CHECK(handle.reload_error().find("cannot read") != std::string::npos);
    CHECK(handle.reload_error().find("NotFound") != std::string::npos);
    CHECK(*handle.get() == "kept");
}

TEST_CASE("Asset reload: closing the world cancels a reload and it never commits", "[integration]") {
    ScratchDir scratch;
    const Guid id = Guid::generate();
    write_cooked(scratch.path(), id, write_cooked_container("old"));
    JobSystem jobs(2);
    Probe probe;
    auto manager = make_text_manager(jobs, scratch.path(), probe);
    auto handle = manager.request(id);
    jobs.wait_idle();
    REQUIRE(handle.ready());

    write_cooked(scratch.path(), id, write_cooked_container("new"));
    probe.gate.hold();
    REQUIRE(manager.reload(handle));
    REQUIRE(wait_until([&probe] { return probe.decodes.load() == 2; }));
    manager.close();
    probe.gate.release();
    jobs.wait_idle();

    CHECK_FALSE(manager.reload(handle));
    CHECK(manager.apply_reloads() == 0);
    CHECK(*handle.get() == "old");
}
