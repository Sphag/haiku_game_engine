#include "engine/world/origin.hpp"

#include "engine/core/assert.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <utility>

namespace engine::world {
namespace {

bool finite(const glm::dvec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

}  // namespace

glm::dvec3 OriginShift::delta() const noexcept {
    return to - from;
}

glm::vec3 OriginShift::rebase_local(glm::vec3 local) const noexcept {
    return glm::vec3(glm::dvec3(local) - delta());
}

glm::mat4 OriginShift::rebase_matrix(const glm::mat4& matrix) const noexcept {
    const glm::dmat4 translation = glm::translate(glm::dmat4(1.0), delta());
    return glm::mat4(glm::dmat4(matrix) * translation);
}

WorldOrigin::WorldOrigin(double rebase_distance, glm::dvec3 origin) : rebase_distance_(rebase_distance), origin_(origin) {
    ENGINE_ASSERT(std::isfinite(rebase_distance_) && rebase_distance_ > 0.0, "the rebase distance must be finite and positive");
    ENGINE_ASSERT(finite(origin_), "the origin must be finite");
}

void WorldOrigin::subscribe(std::shared_ptr<OriginSubscriber> subscriber) {
    ENGINE_ASSERT(subscriber != nullptr, "an origin subscriber must not be null");
    subscribers_.push_back(std::move(subscriber));
}

std::optional<OriginShift> WorldOrigin::update(const glm::dvec3& camera) {
    ENGINE_ASSERT(finite(camera), "the camera position must be finite");
    if (glm::distance(camera, origin_) <= rebase_distance_) {
        return std::nullopt;
    }
    const OriginShift shift{origin_, camera};
    origin_ = camera;
    ++rebases_;
    for (const std::shared_ptr<OriginSubscriber>& subscriber : subscribers_) {
        subscriber->on_origin_shifted(shift);
    }
    return shift;
}

glm::dvec3 WorldOrigin::origin() const noexcept {
    return origin_;
}

double WorldOrigin::rebase_distance() const noexcept {
    return rebase_distance_;
}

std::uint64_t WorldOrigin::rebases() const noexcept {
    return rebases_;
}

glm::vec3 WorldOrigin::to_local(const glm::dvec3& world) const noexcept {
    return glm::vec3(world - origin_);
}

glm::dvec3 WorldOrigin::to_world(const glm::vec3& local) const noexcept {
    return glm::dvec3(local) + origin_;
}

}  // namespace engine::world
