// Large-world origin (US-050) on the CPU: rebasing events, single-precision local positions far from the origin, rebased matrices, a
// physics-style subscriber, and a 20 km route across rebasing boundaries that keeps transforms, handles, cells, and picking. These
// need no GPU and run on every operating system.
#include "engine/render/camera.hpp"
#include "engine/render/frame_snapshot.hpp"
#include "engine/render/mesh.hpp"
#include "engine/scene/scene_graph.hpp"
#include "engine/scene/transform.hpp"
#include "engine/world/origin.hpp"
#include "engine/world/partition.hpp"

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using engine::ecs::EntityHandle;
using engine::render::Camera;
using engine::render::compute_bounds;
using engine::render::extract_frame;
using engine::render::MeshBounds;
using engine::render::RenderableRef;
using engine::render::make_box_mesh;
using engine::scene::SceneGraph;
using engine::scene::Transform;
using engine::world::cell_coord_at;
using engine::world::CellCoord;
using engine::world::OriginShift;
using engine::world::OriginSubscriber;
using engine::world::WorldDesc;
using engine::world::WorldOrigin;

namespace {

// Records every shift it receives, and appends its name to a shared log so the call order is visible.
class RecordingSubscriber : public OriginSubscriber {
public:
    RecordingSubscriber(std::string name, std::vector<std::string>* log) : name_(std::move(name)), log_(log) {}

    void on_origin_shifted(const OriginShift& shift) override {
        shifts.push_back(shift);
        log_->push_back(name_);
    }

    std::vector<OriginShift> shifts;

private:
    std::string name_;
    std::vector<std::string>* log_;
};

// A physics-style adapter: it keeps each body's position in single precision, relative to the origin, and moves it by the delta of
// every shift. Its bodies' authored world positions are kept in double, outside the adapter.
class BodyAdapter : public OriginSubscriber {
public:
    void on_origin_shifted(const OriginShift& shift) override {
        for (glm::vec3& local : locals) {
            local = shift.rebase_local(local);
        }
        ++shifts;
    }

    std::vector<glm::vec3> locals;
    std::uint64_t shifts = 0;
};

}  // namespace

TEST_CASE("The origin holds inside the rebase distance and moves to the camera beyond it", "[unit]") {
    WorldOrigin origin(2048.0);
    CHECK(origin.origin() == glm::dvec3(0.0));

    CHECK_FALSE(origin.update(glm::dvec3(2048.0, 0.0, 0.0)).has_value());  // exactly at the distance: no rebase
    CHECK(origin.rebases() == 0);

    const auto shift = origin.update(glm::dvec3(2048.5, 0.0, -10.0));
    REQUIRE(shift.has_value());
    CHECK(shift->from == glm::dvec3(0.0));
    CHECK(shift->to == glm::dvec3(2048.5, 0.0, -10.0));
    CHECK(origin.origin() == glm::dvec3(2048.5, 0.0, -10.0));
    CHECK(origin.rebases() == 1);

    // The origin is now the camera, so a camera that stays put never rebases again.
    CHECK_FALSE(origin.update(glm::dvec3(2048.5, 0.0, -10.0)).has_value());
    CHECK(origin.rebases() == 1);
}

TEST_CASE("Subscribers receive each rebase once, in subscription order, with the exact delta", "[unit]") {
    std::vector<std::string> log;
    auto first = std::make_shared<RecordingSubscriber>("first", &log);
    auto second = std::make_shared<RecordingSubscriber>("second", &log);

    WorldOrigin origin;
    origin.subscribe(first);
    origin.subscribe(second);

    origin.update(glm::dvec3(5000.0, 0.0, 0.0));
    origin.update(glm::dvec3(5100.0, 0.0, 0.0));  // within the distance of the new origin: no event
    origin.update(glm::dvec3(9000.0, 0.0, 0.0));

    REQUIRE(first->shifts.size() == 2);
    REQUIRE(second->shifts.size() == 2);
    CHECK(first->shifts[0].delta() == glm::dvec3(5000.0, 0.0, 0.0));
    CHECK(first->shifts[1].from == glm::dvec3(5000.0, 0.0, 0.0));
    CHECK(first->shifts[1].to == glm::dvec3(9000.0, 0.0, 0.0));
    CHECK(second->shifts[1].delta() == glm::dvec3(4000.0, 0.0, 0.0));
    CHECK(log == std::vector<std::string>{"first", "second", "first", "second"});
}

TEST_CASE("Single-precision local positions stay exact far from the origin, where a plain float does not", "[unit]") {
    // At 1e6 m a float has a spacing of 0.0625 m, so the position 0.3 m past the camera rounds to 0.3125 m.
    const glm::dvec3 camera(1.0e6, 0.0, -1.0e6);
    const glm::dvec3 prop = camera + glm::dvec3(0.3, 0.0, 0.0);

    const float naive = static_cast<float>(prop.x) - static_cast<float>(camera.x);
    CHECK(std::abs(naive - 0.3f) > 1.0e-3f);

    WorldOrigin origin;
    origin.update(camera);
    const glm::vec3 local = origin.to_local(prop);
    CHECK(std::abs(local.x - 0.3f) < 1.0e-7f);
    CHECK(glm::distance(origin.to_world(local), prop) < 1.0e-6);
}

TEST_CASE("A rebased shift keeps local positions and history matrices consistent with the world", "[unit]") {
    const OriginShift shift{glm::dvec3(100.0, 0.0, -50.0), glm::dvec3(1600.0, 0.0, -750.0)};
    const glm::dvec3 delta = shift.delta();
    CHECK(delta == glm::dvec3(1500.0, 0.0, -700.0));

    // A point 10, 2, 5 m from the old origin is 10 - 1500, 2, 5 + 700 from the new one.
    const glm::vec3 old_local(10.0f, 2.0f, 5.0f);
    const glm::vec3 new_local = shift.rebase_local(old_local);
    CHECK(glm::distance(glm::dvec3(new_local), glm::dvec3(old_local) - delta) < 1.0e-3);

    // A matrix that maps old-local points to clip space maps the same world point from the new-local position after the rebase.
    const glm::mat4 old_matrix = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f)) *
                                 glm::rotate(glm::mat4(1.0f), 0.7f, glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)));
    const glm::mat4 new_matrix = shift.rebase_matrix(old_matrix);
    const glm::vec4 from_old = old_matrix * glm::vec4(old_local, 1.0f);
    const glm::vec4 from_new = new_matrix * glm::vec4(new_local, 1.0f);
    CHECK(glm::distance(from_old, from_new) < 1.0e-2f);
}

TEST_CASE("A physics-style adapter stays consistent with the world across rebases through its subscription", "[unit]") {
    WorldOrigin origin;
    auto adapter = std::make_shared<BodyAdapter>();
    origin.subscribe(adapter);

    // Bodies are authored in world space, far from the origin. The adapter receives them as single-precision local positions.
    const std::vector<glm::dvec3> authored = {
        glm::dvec3(1.0e6 + 3.0, 0.5, 2.0e5 + 1.25),
        glm::dvec3(1.0e6 + 2500.0, 12.0, 2.0e5 + 1250.0),  // on the route, so the camera passes it
    };
    origin.update(glm::dvec3(1.0e6, 0.0, 2.0e5));
    for (const glm::dvec3& body : authored) {
        adapter->locals.push_back(origin.to_local(body));
    }

    // A route of 6 km in 1 m steps crosses about three rebases.
    for (int step = 1; step <= 6000; ++step) {
        origin.update(glm::dvec3(1.0e6 + step, 0.0, 2.0e5 + 0.5 * step));
        for (std::size_t i = 0; i < authored.size(); ++i) {
            CHECK(glm::distance(origin.to_world(adapter->locals[i]), authored[i]) < 1.0e-3);
        }
    }
    CHECK(adapter->shifts == origin.rebases());
    CHECK(origin.rebases() >= 2);
}

TEST_CASE("A far camera selects the cell that owns its exact position", "[unit]") {
    // 256 * 3907 = 1000192. A camera 0.1 mm short of that line is in cell 3906; a float camera would round onto the line and report 3907.
    WorldDesc desc;
    const glm::dvec2 just_before(1000192.0 - 1.0e-4, 0.0);
    CHECK(cell_coord_at(desc, just_before) == CellCoord{3906, 0});
    CHECK(cell_coord_at(desc, glm::dvec2(1000192.0, 0.0)) == CellCoord{3907, 0});
    CHECK(static_cast<float>(just_before.x) == 1000192.0f);
}

TEST_CASE("A 20 km route across rebasing boundaries keeps transforms, handles, cells, and picking", "[unit]") {
    constexpr double kStart = 1.0e6;
    constexpr double kZ = 5.0e5;
    constexpr int kSteps = 40000;  // 0.5 m per step: 20 km
    constexpr int kPropCount = 42;  // one every 500 m along the route, from the start to past its end
    constexpr int kSampleEvery = 200;

    SceneGraph graph;
    std::vector<EntityHandle> handles;
    std::vector<glm::dvec3> props;
    for (int k = 0; k < kPropCount; ++k) {
        const glm::dvec3 position(kStart + 500.0 * k, 0.0, kZ);
        Transform transform{};
        transform.position = position;
        const auto node = graph.create(transform);
        REQUIRE(node.ok());
        handles.push_back(node.value());
        props.push_back(position);
    }
    const std::vector<MeshBounds> bounds = {compute_bounds(make_box_mesh(glm::vec3(1.0f)))};
    std::vector<RenderableRef> renderables;
    for (const EntityHandle& handle : handles) {
        RenderableRef renderable{};
        renderable.node = handle;
        renderables.push_back(renderable);
    }

    // The camera faces +X: a rotation of -90 degrees about +Y maps the default forward (-Z) onto +X.
    const glm::dquat facing = glm::angleAxis(-glm::pi<double>() * 0.5, glm::dvec3(0.0, 1.0, 0.0));
    const WorldDesc desc;

    WorldOrigin origin;
    std::uint64_t samples = 0;
    std::uint64_t picked = 0;
    for (int step = 0; step <= kSteps; ++step) {
        const glm::dvec3 camera(kStart + 0.5 * step, 0.0, kZ);
        origin.update(camera);

        // The camera's own offset from the origin never exceeds the rebase distance.
        CHECK(glm::length(glm::dvec3(origin.to_local(camera))) <= origin.rebase_distance() + 1.0e-3);

        if (step % kSampleEvery != 0) {
            continue;
        }
        ++samples;

        // Transforms are authored in double and untouched by rebasing: every node's world position is exact.
        for (int k = 0; k < kPropCount; ++k) {
            REQUIRE(graph.alive(handles[k]));
            const glm::dmat4* matrix = graph.world_matrix(handles[k]);
            REQUIRE(matrix != nullptr);
            CHECK(glm::dvec3((*matrix)[3]) == props[k]);
        }

        // The prop ahead of the camera, in single precision relative to the origin, is within float rounding of the exact offset.
        const int ahead = static_cast<int>(std::floor((camera.x - kStart) / 500.0)) + 1;
        REQUIRE(ahead < kPropCount);
        const glm::dvec3 exact = props[ahead] - origin.origin();
        const glm::dvec3 local(origin.to_local(props[ahead]));
        CHECK(glm::length(local - exact) <= 2.0e-6 * glm::length(exact) + 1.0e-7);

        // Picking: the snapshot's pick ids resolve to the same entity handle before and after the rebase.
        Camera pose;
        pose.position = camera;
        pose.orientation = facing;
        pose.far_plane = 1000.0;
        REQUIRE(glm::length(pose.forward() - glm::dvec3(1.0, 0.0, 0.0)) < 1.0e-9);
        const auto snapshot = extract_frame(graph, pose, bounds, renderables);
        bool found = false;
        for (const auto& item : snapshot.items()) {
            if (item.pick_id == 0) {
                continue;
            }
            const auto entity = snapshot.entity_of(item.pick_id);
            REQUIRE(entity.has_value());
            if (*entity == handles[ahead]) {
                found = true;
            }
        }
        CHECK(found);
        picked += found ? 1 : 0;

        // Streaming cells follow the exact camera position, whatever the origin is.
        const auto cell = cell_coord_at(desc, glm::dvec2(camera.x, camera.z));
        CHECK(cell.x == static_cast<std::int32_t>(std::floor(camera.x / 256.0)));
        CHECK(cell.z == static_cast<std::int32_t>(std::floor(camera.z / 256.0)));
    }

    CHECK(samples == 201);
    CHECK(picked == samples);
    CHECK(origin.rebases() >= 9);  // about one per 2 km of route
}
