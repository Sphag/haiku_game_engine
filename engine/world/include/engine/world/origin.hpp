#pragma once

// Large-world origin (US-050). Authored world positions are double (engine_scene's Transform), and they are never rewritten by a
// rebase: an entity's world transform, its handle, and its cell stay the same when the origin moves. The origin is an anchor for
// data that is kept in single precision across frames, such as physics bodies and matrices held from the previous frame. Those
// values are float(world - origin), whose spacing is at most 0.24 mm within the rebase distance. When the
// camera moves more than the rebase distance from the origin, the origin moves to the camera and every subscriber receives an
// OriginShift, so it can rewrite its single-precision data by the same delta.
//
// The rebase is an event between frames, like streaming's safe point. A subscriber must not call back into the origin from its
// callback. The renderer needs no rebase: its frame snapshot is already camera-relative in double, and its previous-frame matrices
// are relative to the previous camera (frame_snapshot.hpp), so neither changes when the origin moves.

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace engine::world {

inline constexpr double kDefaultRebaseDistance = 2048.0;  // meters; the single-precision spacing here is 0.24 mm

// One rebase. Positions are world meters. The delta is the amount a single-precision local position written against `from` must be
// moved by to be written against `to`.
struct OriginShift {
    glm::dvec3 from{0.0};
    glm::dvec3 to{0.0};

    [[nodiscard]] glm::dvec3 delta() const noexcept;

    // A local position (world - from) rewritten against `to` (world - to), in single precision.
    [[nodiscard]] glm::vec3 rebase_local(glm::vec3 local) const noexcept;

    // A matrix that maps local positions relative to `from` to clip space, rewritten to map positions relative to `to`. Each
    // point p relative to `to` lies at p + delta relative to `from`, so the result is M * translate(delta).
    [[nodiscard]] glm::mat4 rebase_matrix(const glm::mat4& matrix) const noexcept;
};

// Receives every rebase. The default callback does nothing, so a subscriber overrides only what it needs. A physics adapter is a
// subscriber: it moves its bodies' single-precision positions by the shift's delta, and keeps the entity handles it holds.
class OriginSubscriber {
public:
    virtual ~OriginSubscriber() = default;

    virtual void on_origin_shifted(const OriginShift& shift) { static_cast<void>(shift); }
};

class WorldOrigin {
public:
    // The origin starts at `origin`. `rebase_distance` is the 3D distance, in meters, that the camera may move from the origin before a
    // rebase. It must be finite and positive.
    explicit WorldOrigin(double rebase_distance = kDefaultRebaseDistance, glm::dvec3 origin = glm::dvec3(0.0));

    // Subscribers are called in the order they subscribed.
    void subscribe(std::shared_ptr<OriginSubscriber> subscriber);

    // Between frames. Rebases the origin to the camera when the camera is farther than the rebase distance from it, and returns the
    // shift. Returns nullopt when no rebase happened. The camera must be finite.
    std::optional<OriginShift> update(const glm::dvec3& camera);

    [[nodiscard]] glm::dvec3 origin() const noexcept;
    [[nodiscard]] double rebase_distance() const noexcept;
    [[nodiscard]] std::uint64_t rebases() const noexcept;

    // Single-precision position of a world point relative to the origin, and the reverse.
    [[nodiscard]] glm::vec3 to_local(const glm::dvec3& world) const noexcept;
    [[nodiscard]] glm::dvec3 to_world(const glm::vec3& local) const noexcept;

private:
    double rebase_distance_;
    glm::dvec3 origin_;
    std::uint64_t rebases_ = 0;
    std::vector<std::shared_ptr<OriginSubscriber>> subscribers_;
};

}  // namespace engine::world
