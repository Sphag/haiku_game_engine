#pragma once

// Asynchronous world streaming (US-049), on the CPU. The manager keeps the cells around the camera resident: an active window (9x9
// cells by default) whose cells are drawn and simulated, and a prefetch ring around it whose cells are read and decoded but not yet
// uploaded or activated. Each cell moves through one lifecycle, and subscribers see every step:
//
//   Unloaded -> Reading (IO job) -> Decoding (decode job) -> Loaded -> Ready (GPU upload) -> Active
//                                                                   Active -> Ready -> Unloaded
//
// IO and decode run on the job system and never block update(). Each stage is limited per update by a byte budget: IO, decode,
// and GPU upload. Activation, deactivation, and unload happen only in safe_point(), which must run between frames, so a render
// consumer that snapshots its draw list at begin_frame() never sees a cell change under it. Jobs hold their own copy of the request,
// not a pointer to the manager, so a cancelled or abandoned job writes into memory that outlives it. A cell whose file is missing or
// does not parse ends Failed. It is retried only after it leaves the prefetch ring and comes back.
//
// The manager reads cell files named by the catalog it was built from, so the cell GUIDs and coordinates come from the authored
// world. Subscribers receive the decoded cell as a shared pointer, so they may keep it past unload. A subscriber must not call back
// into the manager from its callbacks.

#include "engine/core/jobs.hpp"
#include "engine/core/path.hpp"
#include "engine/world/partition.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::world {

namespace detail {
struct StreamingState;  // defined in streaming.cpp
}

enum class CellPhase : std::uint8_t {
    Unloaded,   // not resident; a cell that is not wanted, or that was never requested
    Reading,    // IO job in flight
    Decoding,   // decode job in flight
    Loaded,     // decoded and resident on the CPU; waiting for the GPU upload budget
    Ready,      // uploaded; activated at the next safe point if it is inside the active window
    Active,     // inside the active window: drawn and simulated
    Failed,     // the file is missing or does not parse; see StreamingManager::error()
};

// Bytes each stage may spend per update. A stage always starts its first item even when that item is larger than the budget, so a
// large cell cannot stall the stream.
struct StreamingBudget {
    std::size_t io_bytes_per_update = 4u * 1024u * 1024u;
    std::size_t decode_bytes_per_update = 4u * 1024u * 1024u;
    std::size_t gpu_upload_bytes_per_update = 16u * 1024u * 1024u;
};

// Chebyshev radii in cells around the camera's cell. The default active window is 9x9, and the prefetch ring extends one cell beyond it.
struct StreamingWindow {
    std::int32_t active_radius = 4;
    std::int32_t prefetch_radius = 5;
};

using CellData = std::shared_ptr<const WorldCell>;

// Receives the lifecycle of every cell. The default callbacks do nothing, so a subscriber overrides only what it needs. Future physics
// and script adapters are subscribers too.
class CellSubscriber {
public:
    virtual ~CellSubscriber() = default;

    virtual void on_loaded(const CellData& cell) { static_cast<void>(cell); }     // CPU data resident; any time between frames
    virtual void on_uploaded(const CellData& cell) { static_cast<void>(cell); }   // GPU upload done; any time between frames
    virtual void on_activated(const CellData& cell) { static_cast<void>(cell); }  // safe points only
    virtual void on_deactivated(const CellData& cell) { static_cast<void>(cell); }  // safe points only
    virtual void on_unloaded(const CellData& cell) { static_cast<void>(cell); }   // safe points only; after deactivation if active
};

struct StreamingStats {
    std::size_t active_cells = 0;           // Active now
    std::size_t resident_cells = 0;         // Loaded, Ready, or Active
    std::size_t peak_resident_cells = 0;
    std::size_t resident_cpu_bytes = 0;     // serialized size of the resident cells
    std::size_t peak_resident_cpu_bytes = 0;
    std::size_t gpu_bytes = 0;              // estimated upload size of the uploaded cells
    std::size_t peak_gpu_bytes = 0;
    std::size_t transient_bytes = 0;        // file text and decode input held by requests in flight
    std::size_t peak_transient_bytes = 0;
    std::size_t io_bytes_last_update = 0;
    std::size_t decode_bytes_last_update = 0;
    std::size_t gpu_bytes_last_update = 0;
    std::size_t loads = 0;                  // cells that reached Loaded
    std::size_t uploads = 0;
    std::size_t activations = 0;
    std::size_t deactivations = 0;
    std::size_t unloads = 0;
    std::size_t failed_cells = 0;           // failed requests, counted once per attempt
    std::size_t cancelled_requests = 0;
    std::size_t teleports = 0;              // camera moves whose new window does not overlap the old one
};

class StreamingManager {
public:
    // `directory` holds the cell files written by save_dirty. The catalog is read once here, so the manager keeps no reference to it.
    StreamingManager(core::JobSystem& jobs, const core::Path& directory, const World& catalog, StreamingWindow window = {},
                     StreamingBudget budget = {});
    // Cancels requests in flight, waits for them, and unloads every resident cell through the subscribers.
    ~StreamingManager();

    StreamingManager(const StreamingManager&) = delete;
    StreamingManager& operator=(const StreamingManager&) = delete;
    StreamingManager(StreamingManager&&) = delete;
    StreamingManager& operator=(StreamingManager&&) = delete;

    void subscribe(std::shared_ptr<CellSubscriber> subscriber);

    // Residency overrides for the editor (US-072), between frames. A pinned cell stays resident outside the prefetch ring. A suppressed
    // cell is never resident, even inside the active window, and its in-flight request is cancelled; suppression wins over a pin. Both
    // apply at the next update() and safe_point(). This drops only the streamed copy; authored data lives in the caller's World.
    void set_pinned(CellCoord coord, bool pinned);
    void set_suppressed(CellCoord coord, bool suppressed);
    [[nodiscard]] bool pinned(CellCoord coord) const;
    [[nodiscard]] bool suppressed(CellCoord coord) const;

    // Non-blocking, between frames. Cancels requests that left the prefetch ring, starts reads and decodes within budget, and
    // uploads the cells inside the active window within the GPU budget.
    void update(glm::dvec2 camera_xz);

    void begin_frame();
    void end_frame();
    // Between frames only. Activates ready cells in the active window, deactivates cells that left it, and unloads cells outside the
    // prefetch ring.
    void safe_point();

    [[nodiscard]] CellPhase phase(CellCoord coord) const;
    [[nodiscard]] std::string error(CellCoord coord) const;  // empty unless the cell is Failed
    [[nodiscard]] std::vector<CellCoord> active_cells() const;
    [[nodiscard]] const StreamingStats& stats() const noexcept;

private:
    std::unique_ptr<detail::StreamingState> impl_;
};

}  // namespace engine::world
