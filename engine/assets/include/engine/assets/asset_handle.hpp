#pragma once

// Runtime asset handles (US-024): asynchronous access to cooked resources. request() returns at once. A job on the JobSystem
// reads the cooked file, checks its container, and decodes it, so the calling frame never waits for the disk or the decoder.
//
// States. A handle is Loading, Ready, or Failed. get() returns the payload when Ready and the manager's fallback otherwise,
// so a consumer always has something to draw. error() says why a load failed.
//
// Lifetime. The manager keeps an entry only while a handle refers to it. Copying a handle shares the entry. When the last
// handle is released, a load still running is cancelled and its result is dropped, and the payload is freed. A later
// request loads the asset again, which is how a reload works. A failed entry is not retried while a handle still refers to
// it, so a broken file cannot make a consumer retry in a loop.
//
// Publication. The decoder returns a whole payload or an error. The job publishes under the entry's lock, and only while the
// load is still pending and not cancelled. A cancelled, released, or failed load never replaces its state, and no partial
// payload is visible.
//
// Reloads (US-025). reload() decodes the cooked file again on a job while the current payload stays Ready and visible. A
// successful decode is staged, not published. commit_reload() (or apply_reloads() for every live entry) moves the staged
// payload in at a safe point the owner chooses, such as the frame boundary. A failed reload keeps the current payload and
// records the reason in reload_error(), so a bad rebuild never replaces a working asset. A reload is refused while another
// is in flight, and every reload is cancelled by close().
//
// Threads. One thread calls request(), reload(), apply_reloads(), and close(), and live_entries(). Handles may be read,
// committed, and released from any thread.
// Closing. close() is the world-closure hook. It cancels every load that has not published and refuses later requests. The
// JobSystem must outlive the manager's requests.

#include "engine/assets/cooked_container.hpp"
#include "engine/core/assert.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/jobs.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::assets {

enum class LoadState {
    Loading,  // a job is reading or decoding the cooked file
    Ready,    // the payload is published
    Failed,   // the load failed or was cancelled; error() says why
};

[[nodiscard]] constexpr std::string_view to_string(LoadState state) noexcept {
    switch (state) {
        case LoadState::Loading:
            return "loading";
        case LoadState::Ready:
            return "ready";
        case LoadState::Failed:
            return "failed";
    }
    return "unknown";
}

namespace detail {

// One load, shared by every handle to it. All fields except guid_ are guarded by mutex_.
template <typename T>
class AssetEntry {
public:
    AssetEntry(core::Guid guid, std::shared_ptr<const T> fallback) : guid_(guid), fallback_(std::move(fallback)) {}

    // The last handle is gone. A load still running is cancelled, so it stops early and its result is dropped.
    ~AssetEntry() {
        if (job_.valid()) {
            job_.cancel();
        }
        if (reload_job_.valid()) {
            reload_job_.cancel();
        }
    }

    AssetEntry(const AssetEntry&) = delete;
    AssetEntry& operator=(const AssetEntry&) = delete;

    [[nodiscard]] const core::Guid& guid() const noexcept {
        return guid_;
    }

    // Called once, right after the load job is submitted. No handle exists yet, so the job cannot be observed early.
    void attach(core::JobHandle job) {
        const std::lock_guard lock(mutex_);
        job_ = std::move(job);
    }

    // Called from the load job. Publishes only while the load is pending and the job was not cancelled.
    void publish(core::Result<T, std::string> result, const core::CancellationToken& token) {
        const std::lock_guard lock(mutex_);
        if (state_ != LoadState::Loading || token.cancellation_requested()) {
            return;
        }
        if (result.ok()) {
            payload_ = std::make_shared<const T>(std::move(result).value());
            state_ = LoadState::Ready;
        } else {
            fail_locked(result.error());
        }
    }

    // Ends a pending load with a reason and cancels its job. A load that already published keeps its state.
    void fail(std::string reason) {
        core::JobHandle job;
        {
            const std::lock_guard lock(mutex_);
            if (state_ != LoadState::Loading) {
                return;
            }
            fail_locked(std::move(reason));
            job = job_;
        }
        if (job.valid()) {
            job.cancel();
        }
    }

    [[nodiscard]] LoadState state() {
        const std::lock_guard lock(mutex_);
        settle_locked();
        return state_;
    }

    [[nodiscard]] std::string error() {
        const std::lock_guard lock(mutex_);
        settle_locked();
        return error_;
    }

    // The payload when Ready, otherwise the fallback.
    [[nodiscard]] std::shared_ptr<const T> value() {
        const std::lock_guard lock(mutex_);
        settle_locked();
        return state_ == LoadState::Ready ? payload_ : fallback_;
    }

    // Starts a reload. Returns false unless the entry is Ready and no reload is in flight.
    [[nodiscard]] bool begin_reload() {
        const std::lock_guard lock(mutex_);
        settle_locked();
        if (state_ != LoadState::Ready || reloading_) {
            return false;
        }
        reloading_ = true;
        reload_error_.clear();
        return true;
    }

    // Called once, right after the reload job is submitted.
    void attach_reload(core::JobHandle job) {
        const std::lock_guard lock(mutex_);
        reload_job_ = std::move(job);
    }

    // Called from the reload job. Stages the decoded payload, or records the failure and keeps the current payload.
    void stage(core::Result<T, std::string> result, const core::CancellationToken& token) {
        const std::lock_guard lock(mutex_);
        if (!reloading_ || token.cancellation_requested()) {
            return;
        }
        reloading_ = false;
        if (result.ok()) {
            staged_ = std::make_shared<const T>(std::move(result).value());
        } else {
            reload_error_ = result.error();
        }
    }

    // Moves a staged payload in. Returns false when nothing is staged.
    [[nodiscard]] bool commit_reload() {
        const std::lock_guard lock(mutex_);
        settle_reload_locked();
        if (!staged_) {
            return false;
        }
        payload_ = std::move(staged_);
        staged_.reset();
        return true;
    }

    [[nodiscard]] bool reloading() {
        const std::lock_guard lock(mutex_);
        settle_reload_locked();
        return reloading_;
    }

    [[nodiscard]] std::string reload_error() {
        const std::lock_guard lock(mutex_);
        settle_reload_locked();
        return reload_error_;
    }

    // Cancels a reload in flight. Its result is dropped, and the current payload stays.
    void cancel_reload() {
        core::JobHandle job;
        {
            const std::lock_guard lock(mutex_);
            job = reload_job_;
        }
        if (job.valid()) {
            job.cancel();
        }
    }

private:
    void fail_locked(std::string reason) {
        state_ = LoadState::Failed;
        error_ = std::move(reason);
    }

    // A reload whose job ended without staging is recorded as failed, so reloading() never stays true. Caller holds mutex_.
    void settle_reload_locked() {
        if (!reloading_ || !reload_job_.valid() || !core::is_terminal(reload_job_.state())) {
            return;
        }
        reloading_ = false;
        reload_error_ = reload_job_.error();
        if (reload_error_.empty()) {
            reload_error_ = "the reload ended without a result";
        }
    }

    // A load whose job ended without publishing fails here. That covers a job the JobSystem cancelled at shutdown and a
    // job that threw, so a handle never waits for a result that will not arrive. Caller holds mutex_.
    void settle_locked() {
        if (state_ != LoadState::Loading || !job_.valid() || !core::is_terminal(job_.state())) {
            return;
        }
        std::string reason = job_.error();
        if (reason.empty()) {
            reason = "the load ended without a result";
        }
        fail_locked(std::move(reason));
    }

    const core::Guid guid_;
    const std::shared_ptr<const T> fallback_;
    mutable std::mutex mutex_;
    LoadState state_ = LoadState::Loading;
    std::shared_ptr<const T> payload_;
    std::string error_;
    core::JobHandle job_;
    bool reloading_ = false;
    std::shared_ptr<const T> staged_;
    std::string reload_error_;
    core::JobHandle reload_job_;
};

// Runs on a job. Reads the file and checks its container, then decodes the payload. The result is complete or it is an
// error, so a caller never sees a half-built value.
template <typename DecodeFn, typename T>
core::Result<T, std::string> load_cooked(const core::Path& path, const DecodeFn& decode, const core::CancellationToken& token) {
    auto bytes = read_cooked_file(path);
    if (!bytes.ok()) {
        return core::failure(bytes.error());
    }
    if (token.cancellation_requested()) {
        return core::failure(std::string("cancelled before decoding"));
    }
    auto payload = read_cooked_container(bytes.value());
    if (!payload.ok()) {
        return core::failure(path.str() + ": " + payload.error());
    }
    auto decoded = decode(payload.value());
    if (!decoded.ok()) {
        return core::failure(path.str() + ": " + decoded.error());
    }
    return core::success(std::move(decoded).value());
}

}  // namespace detail

template <typename T>
class AssetManager;

// A reference to one load. Copies share it. An empty handle is Failed and get() returns null.
template <typename T>
class AssetHandle {
public:
    AssetHandle() = default;

    [[nodiscard]] bool valid() const noexcept {
        return entry_ != nullptr;
    }

    [[nodiscard]] LoadState state() const {
        return entry_ ? entry_->state() : LoadState::Failed;
    }

    [[nodiscard]] bool ready() const {
        return state() == LoadState::Ready;
    }

    // Empty unless the state is Failed.
    [[nodiscard]] std::string error() const {
        return entry_ ? entry_->error() : std::string("the handle is empty");
    }

    // The payload when Ready, the fallback otherwise, and null for an empty handle.
    [[nodiscard]] std::shared_ptr<const T> get() const {
        return entry_ ? entry_->value() : nullptr;
    }

    [[nodiscard]] core::Guid guid() const {
        return entry_ ? entry_->guid() : core::Guid{};
    }

    // True while a reload job runs or a result waits for commit_reload().
    [[nodiscard]] bool reloading() const {
        return entry_ && entry_->reloading();
    }

    // The reason the last reload failed. Empty when no reload failed since the last one started.
    [[nodiscard]] std::string reload_error() const {
        return entry_ ? entry_->reload_error() : std::string();
    }

    // Moves a staged reload into place. Call it at a safe point. Returns true when the payload changed.
    bool commit_reload() const {
        return entry_ && entry_->commit_reload();
    }

private:
    friend class AssetManager<T>;

    explicit AssetHandle(std::shared_ptr<detail::AssetEntry<T>> entry) noexcept : entry_(std::move(entry)) {}

    std::shared_ptr<detail::AssetEntry<T>> entry_;
};

// Loads the assets of one type. Each request is a job that reads <cooked_root>/<guid>.cooked and decodes it with `decode`.
template <typename T>
class AssetManager {
public:
    using Decoder = std::function<core::Result<T, std::string>(std::string_view payload)>;

    // `fallback` is what handles return until their payload is Ready, so it must not be null.
    AssetManager(core::JobSystem& jobs, core::Path cooked_root, Decoder decode, std::shared_ptr<const T> fallback)
        : jobs_(jobs), cooked_root_(std::move(cooked_root)), decode_(std::move(decode)), fallback_(std::move(fallback)) {
        ENGINE_ASSERT(fallback_ != nullptr, "AssetManager needs a fallback payload");
    }

    // Closing cancels every pending load, so a manager that goes out of scope leaves no work behind.
    ~AssetManager() {
        close();
    }

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;
    AssetManager(AssetManager&&) = delete;
    AssetManager& operator=(AssetManager&&) = delete;

    // Returns a handle to the asset's single load. A second request while the first is alive shares it, so the file is
    // read and decoded once. After the last handle is released, the next request loads the asset again.
    [[nodiscard]] AssetHandle<T> request(const core::Guid& guid) {
        const std::lock_guard lock(mutex_);
        if (auto found = entries_.find(guid); found != entries_.end()) {
            if (auto existing = found->second.lock()) {
                return AssetHandle<T>(std::move(existing));
            }
            entries_.erase(found);
        }

        auto entry = std::make_shared<detail::AssetEntry<T>>(guid, fallback_);
        if (closed_) {
            entry->fail("the asset manager is closed");
            return AssetHandle<T>(std::move(entry));
        }

        auto path = cooked_path(guid);
        if (!path.ok()) {
            entry->fail("cannot name the cooked file: " + std::string(core::to_string(path.error())));
            return AssetHandle<T>(std::move(entry));
        }

        entries_[guid] = entry;
        std::weak_ptr<detail::AssetEntry<T>> weak = entry;
        entry->attach(jobs_.submit([weak, cooked = std::move(path).value(), decode = decode_](
                                       const core::CancellationToken& token) {
            auto result = detail::load_cooked<decltype(decode), T>(cooked, decode, token);
            // A released entry has no owner left to publish to, so the result is dropped with the job.
            if (auto target = weak.lock()) {
                target->publish(std::move(result), token);
            }
        }));
        return AssetHandle<T>(std::move(entry));
    }

    // Decodes the handle's cooked file again. The current payload stays visible until commit_reload(). Returns false when
    // the handle is empty, not Ready, already reloading, or the manager is closed. A false return starts no job.
    [[nodiscard]] bool reload(const AssetHandle<T>& handle) {
        if (!handle.entry_) {
            return false;
        }
        {
            const std::lock_guard lock(mutex_);
            if (closed_) {
                return false;
            }
        }
        if (!handle.entry_->begin_reload()) {
            return false;
        }
        auto path = cooked_path(handle.entry_->guid());
        if (!path.ok()) {
            handle.entry_->stage(core::failure(std::string("cannot name the cooked file")), core::CancellationToken{});
            return false;
        }
        std::weak_ptr<detail::AssetEntry<T>> weak = handle.entry_;
        handle.entry_->attach_reload(jobs_.submit([weak, cooked = std::move(path).value(), decode = decode_](
                                                      const core::CancellationToken& token) {
            auto result = detail::load_cooked<decltype(decode), T>(cooked, decode, token);
            if (auto target = weak.lock()) {
                target->stage(std::move(result), token);
            }
        }));
        return true;
    }

    // The safe point. Commits every staged reload of the live entries and returns how many payloads changed. Handles that
    // are not Ready are skipped. Call it from the thread that owns the frame.
    std::size_t apply_reloads() {
        std::vector<std::shared_ptr<detail::AssetEntry<T>>> live;
        {
            const std::lock_guard lock(mutex_);
            for (const auto& item : entries_) {
                if (auto entry = item.second.lock()) {
                    live.push_back(std::move(entry));
                }
            }
        }
        std::size_t changed = 0;
        for (const auto& entry : live) {
            if (entry->commit_reload()) {
                ++changed;
            }
        }
        return changed;
    }

    // World closure. Every load that has not published fails with a reason and its job is cancelled. Later requests return
    // failed handles without starting a job. Handles already returned keep working, and loaded payloads stay available.
    void close() {
        std::vector<std::shared_ptr<detail::AssetEntry<T>>> live;
        {
            const std::lock_guard lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            for (const auto& item : entries_) {
                if (auto entry = item.second.lock()) {
                    live.push_back(std::move(entry));
                }
            }
            entries_.clear();
        }
        for (const auto& entry : live) {
            entry->fail("the world closed before the asset finished loading");
            entry->cancel_reload();
        }
    }

    [[nodiscard]] bool closed() const {
        const std::lock_guard lock(mutex_);
        return closed_;
    }

    // Entries that still have a handle. Released assets are not counted.
    [[nodiscard]] std::size_t live_entries() const {
        const std::lock_guard lock(mutex_);
        std::size_t count = 0;
        for (const auto& item : entries_) {
            if (!item.second.expired()) {
                ++count;
            }
        }
        return count;
    }

private:
    [[nodiscard]] core::Result<core::Path, core::PathError> cooked_path(const core::Guid& guid) const {
        return cooked_root_.join(guid.to_string() + ".cooked");
    }

    core::JobSystem& jobs_;
    core::Path cooked_root_;
    Decoder decode_;
    std::shared_ptr<const T> fallback_;
    mutable std::mutex mutex_;
    bool closed_ = false;
    std::map<core::Guid, std::weak_ptr<detail::AssetEntry<T>>> entries_;
};

}  // namespace engine::assets
