#include "engine/world/streaming.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/file_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace engine::world {
namespace {

using CellKey = std::pair<std::int32_t, std::int32_t>;

CellKey key_of(CellCoord coord) {
    return {coord.x, coord.z};
}

CellCoord coord_of(const CellKey& key) {
    return CellCoord{key.first, key.second};
}

// Chebyshev distance in cells. The difference is taken in 64 bits so that far-apart coordinates cannot overflow.
std::int64_t cell_distance(CellCoord a, CellCoord b) {
    const auto dx = std::abs(static_cast<std::int64_t>(a.x) - static_cast<std::int64_t>(b.x));
    const auto dz = std::abs(static_cast<std::int64_t>(a.z) - static_cast<std::int64_t>(b.z));
    return std::max(dx, dz);
}

// Estimates. A cell is never costed below the minimum, so a missing or tiny file still takes a share of the budget.
constexpr std::size_t kMinimumCellBytes = 4096;
constexpr std::size_t kGpuBytesPerCell = 256;
constexpr std::size_t kGpuBytesPerObject = 96;

std::size_t gpu_cost(const WorldCell& cell) {
    return kGpuBytesPerCell + kGpuBytesPerObject * cell.objects.size();
}

std::size_t file_bytes(const core::Path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(core::to_native_path(path), ec);
    if (ec) {
        return kMinimumCellBytes;
    }
    return std::max(static_cast<std::size_t>(size), kMinimumCellBytes);
}

// Takes `cost` from a stage's budget. The first item of a stage in an update is always allowed, so an item larger than the budget still
// makes progress; later items wait for the next update once the budget is spent.
bool take(std::size_t& remaining, std::size_t full, std::size_t cost) {
    if (cost > remaining && remaining != full) {
        return false;
    }
    remaining = cost >= remaining ? 0 : remaining - cost;
    return true;
}

}  // namespace

namespace {

// Shared by a cell's IO job and its decode job. A job holds its own pointer, never the manager, so a request that was cancelled or
// abandoned stays valid until the job that writes into it finishes. The main thread reads these fields only after the job is terminal.
struct Request {
    explicit Request(core::Path file) : path(std::move(file)) {}

    core::Path path;
    std::string text;               // written by the IO job, cleared by the decode job
    std::optional<WorldCell> cell;  // written by the decode job
    std::string error;              // written by whichever job fails
};

}  // namespace

namespace detail {

struct StreamingState {
    struct CatalogEntry {
        core::Path path;
        std::size_t file_bytes = 0;
    };

    struct Entry {
        CellPhase phase = CellPhase::Unloaded;
        std::shared_ptr<Request> request;
        core::JobHandle job;
        std::size_t bytes = 0;       // file text while reading, the decode input, then the resident size
        std::size_t gpu_bytes = 0;   // estimated upload size once Ready
        CellData data;
        std::string error;
    };

    explicit StreamingState(core::JobSystem& job_system) : jobs(job_system) {}

    core::JobSystem& jobs;
    WorldDesc desc;
    StreamingWindow window;
    StreamingBudget budget;
    std::map<CellKey, CatalogEntry> catalog;
    std::map<CellKey, Entry> entries;  // cells that are in flight, resident, or failed; an unrequested cell has no entry
    std::set<CellKey> pinned;          // resident outside the prefetch ring (editor load and pin)
    std::set<CellKey> suppressed;      // never resident, even inside the active window (editor unload)
    std::vector<std::shared_ptr<CellSubscriber>> subscribers;
    std::optional<CellCoord> centre;   // the camera's cell at the last update
    bool frame_open = false;
    StreamingStats stats;
};

}  // namespace detail

namespace {

using Impl = detail::StreamingState;
using Entry = Impl::Entry;

// A cell is wanted when it is inside the prefetch ring or pinned, unless it is suppressed. Suppression wins over a pin.
bool wants(const Impl& s, const CellKey& key, std::int64_t distance) {
    if (s.suppressed.count(key) != 0) {
        return false;
    }
    return distance <= s.window.prefetch_radius || s.pinned.count(key) != 0;
}

// A cell is in the active window when it is inside the active radius and not suppressed.
bool in_window(const Impl& s, const CellKey& key, std::int64_t distance) {
    return distance <= s.window.active_radius && s.suppressed.count(key) == 0;
}

void notify_loaded(Impl& s, const CellData& data) {
    for (auto& subscriber : s.subscribers) {
        subscriber->on_loaded(data);
    }
}

void notify_uploaded(Impl& s, const CellData& data) {
    for (auto& subscriber : s.subscribers) {
        subscriber->on_uploaded(data);
    }
}

void notify_activated(Impl& s, const CellData& data) {
    for (auto& subscriber : s.subscribers) {
        subscriber->on_activated(data);
    }
}

void notify_deactivated(Impl& s, const CellData& data) {
    for (auto& subscriber : s.subscribers) {
        subscriber->on_deactivated(data);
    }
}

void notify_unloaded(Impl& s, const CellData& data) {
    for (auto& subscriber : s.subscribers) {
        subscriber->on_unloaded(data);
    }
}

void fail(Impl& s, Entry& entry, std::string message) {
    entry.phase = CellPhase::Failed;
    entry.error = std::move(message);
    entry.request.reset();
    entry.job = {};
    entry.bytes = 0;
    ++s.stats.failed_cells;
}

// Recounts residency from the entries, so the counters cannot drift from the state. Peaks are kept across updates.
void refresh_stats(Impl& s) {
    auto& stats = s.stats;
    stats.active_cells = 0;
    stats.resident_cells = 0;
    stats.resident_cpu_bytes = 0;
    stats.gpu_bytes = 0;
    stats.transient_bytes = 0;
    for (const auto& [key, entry] : s.entries) {
        switch (entry.phase) {
            case CellPhase::Reading:
            case CellPhase::Decoding:
                stats.transient_bytes += entry.bytes;
                break;
            case CellPhase::Loaded:
            case CellPhase::Ready:
            case CellPhase::Active:
                ++stats.resident_cells;
                stats.resident_cpu_bytes += entry.bytes;
                if (entry.phase != CellPhase::Loaded) {
                    stats.gpu_bytes += entry.gpu_bytes;
                }
                if (entry.phase == CellPhase::Active) {
                    ++stats.active_cells;
                }
                break;
            case CellPhase::Unloaded:
            case CellPhase::Failed:
                break;
        }
    }
    stats.peak_resident_cells = std::max(stats.peak_resident_cells, stats.resident_cells);
    stats.peak_resident_cpu_bytes = std::max(stats.peak_resident_cpu_bytes, stats.resident_cpu_bytes);
    stats.peak_gpu_bytes = std::max(stats.peak_gpu_bytes, stats.gpu_bytes);
    stats.peak_transient_bytes = std::max(stats.peak_transient_bytes, stats.transient_bytes);
}

}  // namespace

StreamingManager::StreamingManager(core::JobSystem& jobs, const core::Path& directory, const World& catalog, StreamingWindow window,
                                   StreamingBudget budget)
    : impl_(std::make_unique<Impl>(jobs)) {
    ENGINE_ASSERT(window.active_radius >= 0 && window.prefetch_radius >= window.active_radius,
                  "the prefetch radius must be at least the active radius, which is not negative");
    ENGINE_ASSERT(budget.io_bytes_per_update > 0 && budget.decode_bytes_per_update > 0 && budget.gpu_upload_bytes_per_update > 0,
                  "every streaming budget must be positive");
    impl_->desc = catalog.desc;
    impl_->window = window;
    impl_->budget = budget;
    for (const auto& cell : catalog.cells) {
        auto path = directory.join(cell_file_name(cell));
        ENGINE_ASSERT(path.ok(), "a cell file name is a valid path");
        if (!path.ok()) {
            continue;
        }
        const std::size_t bytes = file_bytes(path.value());
        impl_->catalog.insert_or_assign(key_of(cell.coord), Impl::CatalogEntry{std::move(path.value()), bytes});
    }
}

StreamingManager::~StreamingManager() {
    if (!impl_) {
        return;
    }
    auto& s = *impl_;
    for (auto& [key, entry] : s.entries) {
        if (entry.job.valid()) {
            entry.job.cancel();
        }
    }
    // Jobs are cancelled before the subscribers hear anything, so none of them can finish into a request that is being torn down.
    for (auto& [key, entry] : s.entries) {
        if (entry.job.valid()) {
            entry.job.wait();
        }
    }
    for (auto& [key, entry] : s.entries) {
        if (!entry.data) {
            continue;
        }
        if (entry.phase == CellPhase::Active) {
            notify_deactivated(s, entry.data);
        }
        if (entry.phase == CellPhase::Loaded || entry.phase == CellPhase::Ready || entry.phase == CellPhase::Active) {
            notify_unloaded(s, entry.data);
        }
    }
}

void StreamingManager::subscribe(std::shared_ptr<CellSubscriber> subscriber) {
    ENGINE_ASSERT(subscriber != nullptr, "a streaming subscriber must not be null");
    impl_->subscribers.push_back(std::move(subscriber));
}

void StreamingManager::set_pinned(CellCoord coord, bool pinned) {
    if (pinned) {
        impl_->pinned.insert(key_of(coord));
    } else {
        impl_->pinned.erase(key_of(coord));
    }
}

void StreamingManager::set_suppressed(CellCoord coord, bool suppressed) {
    if (suppressed) {
        impl_->suppressed.insert(key_of(coord));
    } else {
        impl_->suppressed.erase(key_of(coord));
    }
}

bool StreamingManager::pinned(CellCoord coord) const {
    return impl_->pinned.count(key_of(coord)) != 0;
}

bool StreamingManager::suppressed(CellCoord coord) const {
    return impl_->suppressed.count(key_of(coord)) != 0;
}

void StreamingManager::update(glm::dvec2 camera_xz) {
    auto& s = *impl_;
    ENGINE_ASSERT(!s.frame_open, "update() must not run during a frame");
    ENGINE_ASSERT(std::isfinite(camera_xz.x) && std::isfinite(camera_xz.y), "the camera position must be finite");

    const CellCoord centre = cell_coord_at(s.desc, camera_xz);
    const std::int64_t prefetch = s.window.prefetch_radius;
    if (s.centre && cell_distance(*s.centre, centre) > 2 * prefetch) {
        ++s.stats.teleports;  // the new window shares no cell with the old one
    }
    s.centre = centre;
    s.stats.io_bytes_last_update = 0;
    s.stats.decode_bytes_last_update = 0;
    s.stats.gpu_bytes_last_update = 0;

    // Requests that left the prefetch ring are cancelled. Failed cells that left it are forgotten, so a return to the ring retries
    // them. Resident cells stay here until the next safe point unloads them.
    for (auto it = s.entries.begin(); it != s.entries.end();) {
        const bool inside = wants(s, it->first, cell_distance(coord_of(it->first), centre));
        const CellPhase phase = it->second.phase;
        if (!inside && (phase == CellPhase::Reading || phase == CellPhase::Decoding)) {
            it->second.job.cancel();
            ++s.stats.cancelled_requests;
            it = s.entries.erase(it);
        } else if (!inside && phase == CellPhase::Failed) {
            it = s.entries.erase(it);
        } else {
            ++it;
        }
    }

    // The cells in the ring, nearest first. Budgets are spent in this order, so the active window fills before the ring.
    struct Wanted {
        std::int64_t distance;
        CellKey key;
    };
    std::vector<Wanted> wanted;
    for (const auto& [key, catalog_entry] : s.catalog) {
        const std::int64_t distance = cell_distance(coord_of(key), centre);
        if (wants(s, key, distance)) {
            wanted.push_back(Wanted{distance, key});
        }
    }
    std::stable_sort(wanted.begin(), wanted.end(), [](const Wanted& a, const Wanted& b) { return a.distance < b.distance; });

    // Finished IO hands its text to a decode job, within the decode budget. A finished decode becomes Loaded.
    std::size_t decode_remaining = s.budget.decode_bytes_per_update;
    for (const auto& item : wanted) {
        auto found = s.entries.find(item.key);
        if (found == s.entries.end()) {
            continue;
        }
        Entry& entry = found->second;
        if (entry.phase == CellPhase::Reading && is_terminal(entry.job.state())) {
            entry.job.wait();
            if (entry.job.state() == core::JobState::Cancelled) {
                fail(s, entry, "the read was cancelled by the job system");
                continue;
            }
            if (!entry.request->error.empty()) {
                fail(s, entry, entry.request->error);
                continue;
            }
            entry.bytes = entry.request->text.size();
            if (!take(decode_remaining, s.budget.decode_bytes_per_update, entry.bytes)) {
                continue;
            }
            auto request = entry.request;
            entry.job = s.jobs.submit([request](const core::CancellationToken& token) {
                if (token.cancellation_requested()) {
                    return;
                }
                auto cell = parse_cell(request->text);
                if (!cell.ok()) {
                    request->error = cell.error();
                    return;
                }
                request->cell = std::move(cell.value());
                request->text.clear();
            });
            entry.phase = CellPhase::Decoding;
            s.stats.decode_bytes_last_update += entry.bytes;
        } else if (entry.phase == CellPhase::Decoding && is_terminal(entry.job.state())) {
            entry.job.wait();
            if (entry.job.state() == core::JobState::Cancelled || !entry.request->cell) {
                fail(s, entry, entry.request->error.empty() ? "the decode was cancelled by the job system" : entry.request->error);
                continue;
            }
            if (!entry.request->error.empty()) {
                fail(s, entry, entry.request->error);
                continue;
            }
            entry.data = std::make_shared<WorldCell>(std::move(*entry.request->cell));
            entry.request.reset();
            entry.job = {};
            entry.phase = CellPhase::Loaded;
            ++s.stats.loads;
            notify_loaded(s, entry.data);
        }
    }

    // New reads for ring cells that have no entry, nearest first, within the IO budget.
    std::size_t io_remaining = s.budget.io_bytes_per_update;
    for (const auto& item : wanted) {
        if (s.entries.count(item.key) != 0) {
            continue;
        }
        const auto& catalog_entry = s.catalog.at(item.key);
        if (!take(io_remaining, s.budget.io_bytes_per_update, catalog_entry.file_bytes)) {
            continue;
        }
        Entry entry;
        entry.phase = CellPhase::Reading;
        entry.request = std::make_shared<Request>(catalog_entry.path);
        entry.bytes = catalog_entry.file_bytes;
        auto request = entry.request;
        entry.job = s.jobs.submit([request](const core::CancellationToken& token) {
            if (token.cancellation_requested()) {
                return;
            }
            auto text = core::read_text_file(request->path);
            if (!text.ok()) {
                request->error = core::describe(text.error());
                return;
            }
            request->text = std::move(text.value());
        });
        s.stats.io_bytes_last_update += catalog_entry.file_bytes;
        s.entries.emplace(item.key, std::move(entry));
    }

    // GPU upload for loaded cells inside the active window, nearest first, within the upload budget.
    std::size_t gpu_remaining = s.budget.gpu_upload_bytes_per_update;
    for (const auto& item : wanted) {
        if (!in_window(s, item.key, item.distance)) {
            continue;
        }
        auto found = s.entries.find(item.key);
        if (found == s.entries.end() || found->second.phase != CellPhase::Loaded) {
            continue;
        }
        Entry& entry = found->second;
        const std::size_t cost = gpu_cost(*entry.data);
        if (!take(gpu_remaining, s.budget.gpu_upload_bytes_per_update, cost)) {
            continue;
        }
        entry.gpu_bytes = cost;
        entry.phase = CellPhase::Ready;
        ++s.stats.uploads;
        s.stats.gpu_bytes_last_update += cost;
        notify_uploaded(s, entry.data);
    }

    refresh_stats(s);
}

void StreamingManager::begin_frame() {
    ENGINE_ASSERT(!impl_->frame_open, "begin_frame() called twice");
    impl_->frame_open = true;
}

void StreamingManager::end_frame() {
    ENGINE_ASSERT(impl_->frame_open, "end_frame() without begin_frame()");
    impl_->frame_open = false;
}

void StreamingManager::safe_point() {
    auto& s = *impl_;
    ENGINE_ASSERT(!s.frame_open, "safe_point() must not run during a frame");
    if (!s.centre) {
        return;
    }
    for (auto it = s.entries.begin(); it != s.entries.end();) {
        Entry& entry = it->second;
        const std::int64_t distance = cell_distance(coord_of(it->first), *s.centre);

        if (entry.phase == CellPhase::Ready && in_window(s, it->first, distance)) {
            entry.phase = CellPhase::Active;
            ++s.stats.activations;
            notify_activated(s, entry.data);
        }
        if (entry.phase == CellPhase::Active && !in_window(s, it->first, distance)) {
            entry.phase = CellPhase::Ready;
            ++s.stats.deactivations;
            notify_deactivated(s, entry.data);
        }
        const bool resident = entry.phase == CellPhase::Loaded || entry.phase == CellPhase::Ready || entry.phase == CellPhase::Active;
        if (resident && !wants(s, it->first, distance)) {
            if (entry.phase == CellPhase::Active) {
                ++s.stats.deactivations;
                notify_deactivated(s, entry.data);
            }
            const CellData data = entry.data;
            notify_unloaded(s, data);
            ++s.stats.unloads;
            it = s.entries.erase(it);
        } else {
            ++it;
        }
    }
    refresh_stats(s);
}

CellPhase StreamingManager::phase(CellCoord coord) const {
    const auto found = impl_->entries.find(key_of(coord));
    return found == impl_->entries.end() ? CellPhase::Unloaded : found->second.phase;
}

std::string StreamingManager::error(CellCoord coord) const {
    const auto found = impl_->entries.find(key_of(coord));
    if (found == impl_->entries.end() || found->second.phase != CellPhase::Failed) {
        return {};
    }
    return found->second.error;
}

std::vector<CellCoord> StreamingManager::active_cells() const {
    std::vector<CellCoord> active;
    for (const auto& [key, entry] : impl_->entries) {
        if (entry.phase == CellPhase::Active) {
            active.push_back(coord_of(key));
        }
    }
    return active;
}

const StreamingStats& StreamingManager::stats() const noexcept {
    return impl_->stats;
}

}  // namespace engine::world
