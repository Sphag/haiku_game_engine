// CPU checks of the ray-traced effects (US-043): the frame and scene validation, the byte images the pass uploads, the policies (roughness
// cutoff, alpha cutoff), the geometry capability report, and the CPU reference on the reference scene. The reference scene's expectations
// are checked here, so the GPU suite (render_rt_gpu_tests.cpp) compares against values that are known to hold.
#include "render_rt_fixture.hpp"

#include "engine/render/rt_effects.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using engine::render::alpha_passes;

using engine::render::kRtInstanceStride;
using engine::render::kRtParameterBytes;
using engine::render::kRtTriangleStride;
using engine::render::look_at_frame;
using engine::render::make_top_level_instance;
using engine::render::pack_frame_parameters;
using engine::render::pack_instance_table;
using engine::render::pack_triangle_table;
using engine::render::reflection_applies;
using engine::render::RtFrame;
using engine::render::RtReference;
using engine::render::RtInstanceRecord;
using engine::render::RtTriangleRecord;
using engine::render::geometry_supported;
using engine::render::rt_effects_geometry_support;
using engine::render::trace_reference;
using engine::render::tests::kRtFence;
using engine::render::tests::kRtGround;
using engine::render::tests::kRtOccluder;
using engine::render::tests::kRtPillar;
using engine::render::tests::kRtRoughBox;
using engine::render::tests::kRtSize;
using engine::render::tests::rt_checker_mask;
using engine::render::tests::rt_scene;
using engine::render::tests::rt_triangles_of;
using engine::render::tests::rt_view;
using engine::render::tests::rt_box_mesh;
using engine::render::tests::rt_quad_mesh;
using engine::render::validate_frame;
using engine::render::validate_scene_records;

namespace {

float read_float(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    float value = 0.0f;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::uint32_t read_uint(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

// Pixels where the predicate holds, counted over the reference's image.
template <typename Predicate>
std::size_t count_pixels(const RtReference& reference, Predicate predicate) {
    std::size_t count = 0;
    for (std::uint32_t y = 0; y < reference.height; ++y) {
        for (std::uint32_t x = 0; x < reference.width; ++x) {
            if (predicate(reference.index(x, y))) {
                ++count;
            }
        }
    }
    return count;
}

// A reflection of the red pillar: red dominates, the way the pillar's albedo does after Fresnel weighting.
bool red_reflection(const RtReference& reference, std::size_t index) {
    const glm::vec4& value = reference.reflection[index];
    return value.a > 0.5f && value.r > 2.0f * value.g && value.r > 2.0f * value.b;
}

}  // namespace

TEST_CASE("The frame of the reference scene is valid, and invalid frames name their problem", "[unit]") {
    CHECK_FALSE(validate_frame(rt_view()).has_value());

    RtFrame bad = rt_view();
    bad.forward = glm::vec3(0.0f, 0.0f, -2.0f);
    CHECK(validate_frame(bad).has_value());

    bad = rt_view();
    bad.up = bad.forward;
    CHECK(validate_frame(bad).has_value());

    bad = rt_view();
    bad.roughness_cutoff = 1.5f;
    CHECK(validate_frame(bad).has_value());

    bad = rt_view();
    bad.reflection_distance = 0.0f;
    CHECK(validate_frame(bad).has_value());

    bad = rt_view();
    bad.shadow_distance = std::nanf("");
    CHECK(validate_frame(bad).has_value());

    bad = rt_view();
    bad.bias = -0.1f;
    CHECK(validate_frame(bad).has_value());

    bad = rt_view();
    bad.sun_direction = glm::vec3(0.0f);
    CHECK(validate_frame(bad).has_value());

    bad = rt_view();
    bad.fov_y_radians = 0.0f;
    CHECK(validate_frame(bad).has_value());
}

TEST_CASE("The roughness cutoff and the alpha cutoff are inclusive", "[unit]") {
    CHECK(reflection_applies(0.4f, 0.4f));
    CHECK(reflection_applies(0.0f, 0.4f));
    CHECK_FALSE(reflection_applies(0.41f, 0.4f));
    CHECK(alpha_passes(0.5f, 0.5f));
    CHECK(alpha_passes(1.0f, 0.5f));
    CHECK_FALSE(alpha_passes(0.0f, 0.5f));
}

TEST_CASE("The frame image holds the rays' basis, the flags, and the distances at their offsets", "[unit]") {
    RtFrame frame = rt_view();
    frame.shadows = true;
    frame.reflections = false;
    const std::vector<std::uint8_t> bytes = pack_frame_parameters(frame, kRtSize, 32);
    REQUIRE(bytes.size() == kRtParameterBytes);
    CHECK(read_float(bytes, 0) == frame.eye.x);
    CHECK(read_float(bytes, 4) == frame.eye.y);
    CHECK(read_float(bytes, 8) == frame.eye.z);
    // The pass traces with right = cross(forward, up) scaled by tan(fov/2) * aspect, and up scaled by tan(fov/2). For this view that is
    // tan(30 degrees) along the unit right axis, and the right axis of a camera looking down -Z is +X.
    const float tan_half = std::tan(0.5f * frame.fov_y_radians);
    CHECK(std::abs(read_float(bytes, 16) - tan_half * frame.aspect * glm::normalize(glm::cross(frame.forward, frame.up)).x) < 1.0e-6f);
    CHECK(read_uint(bytes, 112) == kRtSize);
    CHECK(read_uint(bytes, 116) == 32u);
    CHECK(read_uint(bytes, 120) == 1u);
    CHECK(read_float(bytes, 124) == frame.roughness_cutoff);
    CHECK(read_float(bytes, 128) == frame.reflection_distance);
    CHECK(read_float(bytes, 132) == frame.shadow_distance);
    CHECK(read_float(bytes, 136) == frame.bias);

    frame.shadows = false;
    frame.reflections = true;
    CHECK(read_uint(pack_frame_parameters(frame, kRtSize, kRtSize), 120) == 2u);
}

TEST_CASE("The instance and triangle tables use the record strides and the offsets of the shader", "[unit]") {
    const engine::render::tests::RtSceneRecords scene = rt_scene(-1.5f);
    const std::vector<std::uint8_t> instances = pack_instance_table(scene.instances);
    CHECK(instances.size() == scene.instances.size() * kRtInstanceStride);
    const std::size_t fence = static_cast<std::size_t>(kRtFence) * kRtInstanceStride;
    CHECK(read_uint(instances, fence + 24) == 1u);  // masked
    CHECK(read_float(instances, fence + 28) == 0.5f);
    CHECK(read_uint(instances, static_cast<std::size_t>(kRtPillar) * kRtInstanceStride + 24) == 0u);
    CHECK(read_float(instances, static_cast<std::size_t>(kRtRoughBox) * kRtInstanceStride + 16) == 0.9f);

    const std::vector<std::uint8_t> triangles = pack_triangle_table(scene.triangles);
    CHECK(triangles.size() == scene.triangles.size() * kRtTriangleStride);
    // The first triangle of the first instance (the ground) starts at its triangle base: position 0 at byte 0, UV 0 at byte 36.
    CHECK(read_float(triangles, 0) == scene.triangles[0].positions[0].x);
    CHECK(read_float(triangles, 36) == scene.triangles[0].uvs[0].x);
}

TEST_CASE("A top-level instance is the row-major 3x4 transform, with the record index as its InstanceID", "[unit]") {
    const RtInstanceRecord record = rt_scene(-1.5f).instances[static_cast<std::size_t>(kRtOccluder)];
    const engine::rhi::TopLevelInstance instance = make_top_level_instance(record, kRtOccluder, engine::rhi::AccelerationStructureHandle{});
    CHECK(instance.instance_id == kRtOccluder);
    // Translation sits in the last column of each row; the scale (0.5, 1, 0.5) is on the diagonal.
    CHECK(instance.transform[3] == -1.5f);
    CHECK(instance.transform[7] == 1.0f);
    CHECK(instance.transform[11] == 1.0f);
    CHECK(instance.transform[0] == 0.5f);
    CHECK(instance.transform[5] == 1.0f);
    CHECK(instance.transform[10] == 0.5f);
}

TEST_CASE("Scene records are checked before they reach the GPU", "[unit]") {
    const engine::render::tests::RtSceneRecords scene = rt_scene(-1.5f);
    CHECK_FALSE(validate_scene_records(scene.instances, scene.triangles).has_value());

    CHECK(validate_scene_records({}, scene.triangles).has_value());

    std::vector<RtInstanceRecord> outside = scene.instances;
    outside.back().triangle_base = static_cast<std::uint32_t>(scene.triangles.size());
    CHECK(validate_scene_records(outside, scene.triangles).has_value());

    std::vector<RtInstanceRecord> empty_rect = scene.instances;
    empty_rect[kRtFence].mask_rect.z = 0.0f;
    CHECK(validate_scene_records(empty_rect, scene.triangles).has_value());

    std::vector<RtInstanceRecord> bad_roughness = scene.instances;
    bad_roughness[kRtGround].roughness = 1.5f;
    CHECK(validate_scene_records(bad_roughness, scene.triangles).has_value());
}

TEST_CASE("The triangle records follow the index order of their mesh", "[unit]") {
    const auto box = rt_triangles_of(rt_box_mesh());
    CHECK(box.size() == 12);
    CHECK(box[0].positions[0] == glm::vec3(-1, -1, -1));
    const auto quad = rt_triangles_of(rt_quad_mesh());
    CHECK(quad.size() == 2);
    CHECK(quad[1].positions[2] == glm::vec3(-1, 1, 0));
}

TEST_CASE("The capability report lists the geometry classes the pass does not trace", "[unit]") {
    CHECK(geometry_supported("alpha-mask triangles"));
    CHECK(geometry_supported("opaque triangle meshes"));
    CHECK_FALSE(geometry_supported("alpha-blended triangles"));
    CHECK_FALSE(geometry_supported("skinned and deforming meshes"));
    CHECK_FALSE(geometry_supported("procedural and AABB primitives"));
    CHECK(geometry_supported("heightfield terrain"));
    CHECK(rt_effects_geometry_support().size() >= 5);
}

TEST_CASE("The reference never hits the pillar from the camera, but reflects it on the ground", "[unit]") {
    const auto scene = rt_scene(-1.5f);
    const RtReference reference = trace_reference(scene.instances, scene.triangles, scene.mask, rt_view(), kRtSize, kRtSize);

    // The pillar is outside the view: no primary ray reaches it.
    CHECK(count_pixels(reference, [&](std::size_t i) { return reference.primary_instance[i] == static_cast<std::int32_t>(kRtPillar); }) == 0);
    // Its reflection is on screen: some ground pixels reflect it.
    const std::size_t reflected = count_pixels(reference, [&](std::size_t i) { return red_reflection(reference, i); });
    INFO("pixels reflecting the off-screen pillar: " << reflected);
    CHECK(reflected >= 10);
}

TEST_CASE("The reference shadows the ground under the occluder, and the shadow moves when the occluder moves", "[unit]") {
    const auto left = rt_scene(-1.5f);
    const auto right = rt_scene(1.5f);
    const RtReference a = trace_reference(left.instances, left.triangles, left.mask, rt_view(), kRtSize, kRtSize);
    const RtReference b = trace_reference(right.instances, right.triangles, right.mask, rt_view(), kRtSize, kRtSize);

    const std::size_t shadowed_a = count_pixels(a, [&](std::size_t i) { return a.shadow[i] < 0.5f; });
    const std::size_t shadowed_b = count_pixels(b, [&](std::size_t i) { return b.shadow[i] < 0.5f; });
    INFO("shadowed pixels: " << shadowed_a << " and " << shadowed_b);
    CHECK(shadowed_a > 0);
    CHECK(shadowed_b > 0);
    const std::size_t moved = count_pixels(a, [&](std::size_t i) { return (a.shadow[i] < 0.5f) != (b.shadow[i] < 0.5f); });
    INFO("pixels whose shadow changes when the occluder moves: " << moved);
    CHECK(moved > 0);
}

TEST_CASE("The fence casts a shadow with holes, and the mask decides what the camera sees through it", "[unit]") {
    const auto masked = rt_scene(-1.5f, true);
    const auto open = rt_scene(-1.5f, false);
    const RtReference with_mask = trace_reference(masked.instances, masked.triangles, masked.mask, rt_view(), kRtSize, kRtSize);
    const RtReference without_mask = trace_reference(open.instances, open.triangles, open.mask, rt_view(), kRtSize, kRtSize);

    // Holes change what the camera sees, and the masked fence casts a different shadow.
    const std::size_t hits_masked = count_pixels(with_mask, [&](std::size_t i) { return with_mask.primary_instance[i] == static_cast<std::int32_t>(kRtFence); });
    const std::size_t hits_open = count_pixels(without_mask, [&](std::size_t i) { return without_mask.primary_instance[i] == static_cast<std::int32_t>(kRtFence); });
    INFO("fence pixels seen with the mask: " << hits_masked << ", without: " << hits_open);
    CHECK(hits_masked > 0);
    CHECK(hits_masked < hits_open);
    const std::size_t shadow_changes = count_pixels(with_mask, [&](std::size_t i) { return (with_mask.shadow[i] < 0.5f) != (without_mask.shadow[i] < 0.5f); });
    INFO("shadow pixels that differ with the mask: " << shadow_changes);
    CHECK(shadow_changes > 0);
}

TEST_CASE("The reference does not reflect a surface above the roughness cutoff", "[unit]") {
    const auto scene = rt_scene(-1.5f);
    const RtReference reference = trace_reference(scene.instances, scene.triangles, scene.mask, rt_view(), kRtSize, kRtSize);
    const std::size_t rough = count_pixels(reference, [&](std::size_t i) { return reference.primary_instance[i] == static_cast<std::int32_t>(kRtRoughBox); });
    CHECK(rough > 0);
    CHECK(count_pixels(reference, [&](std::size_t i) {
              return reference.primary_instance[i] == static_cast<std::int32_t>(kRtRoughBox) && reference.reflection[i].a > 0.0f;
          }) == 0);
}

TEST_CASE("The reflection distance limits the reflection: a short limit loses the pillar", "[unit]") {
    const auto scene = rt_scene(-1.5f);
    RtFrame frame = rt_view();
    const RtReference full = trace_reference(scene.instances, scene.triangles, scene.mask, frame, kRtSize, kRtSize);
    frame.reflection_distance = 0.5f;
    const RtReference limited = trace_reference(scene.instances, scene.triangles, scene.mask, frame, kRtSize, kRtSize);
    CHECK(count_pixels(full, [&](std::size_t i) { return red_reflection(full, i); }) > 0);
    CHECK(count_pixels(limited, [&](std::size_t i) { return red_reflection(limited, i); }) == 0);
}

TEST_CASE("With shadows and reflections off, the reference is the raster fallback: lit everywhere and no reflections", "[unit]") {
    const auto scene = rt_scene(-1.5f);
    RtFrame frame = rt_view();
    frame.shadows = false;
    frame.reflections = false;
    const RtReference off = trace_reference(scene.instances, scene.triangles, scene.mask, frame, kRtSize, kRtSize);
    CHECK(count_pixels(off, [&](std::size_t i) { return off.shadow[i] != 1.0f; }) == 0);
    CHECK(count_pixels(off, [&](std::size_t i) { return off.reflection[i].a != 0.0f; }) == 0);

    const RtReference on = trace_reference(scene.instances, scene.triangles, scene.mask, rt_view(), kRtSize, kRtSize);
    CHECK(count_pixels(on, [&](std::size_t i) { return on.shadow[i] < 0.5f; }) > 0);
}

TEST_CASE("A one-triangle scene is hit at its centre and missed in its corner", "[unit]") {
    // A one-triangle scene: the camera sees the triangle at its centre, so the primary instance is 0. The sun points toward the camera,
    // away from the triangle, so nothing blocks it and the shadow is lit.
    RtInstanceRecord record;
    record.triangle_count = 1;
    RtTriangleRecord triangle;
    triangle.positions = {glm::vec3(-1, -1, 0), glm::vec3(1, -1, 0), glm::vec3(0, 1, 0)};
    triangle.uvs = {glm::vec2(0), glm::vec2(1, 0), glm::vec2(0, 1)};
    RtFrame frame = look_at_frame(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), 1.0f, 1.0f);
    frame.sun_direction = glm::vec3(0.0f, 0.0f, 1.0f);
    frame.reflections = false;
    const RtReference reference = trace_reference({record}, {triangle}, rt_checker_mask(), frame, 8, 8);
    CHECK(reference.primary_instance[reference.index(4, 4)] == 0);
    CHECK(reference.shadow[reference.index(4, 4)] == 1.0f);
    CHECK(reference.primary_instance[reference.index(0, 0)] == -1);
}

TEST_CASE("The frame image carries the previous camera's position and axes at 144 to 208, and a copy of the current one gives no motion", "[unit]") {
    RtFrame frame = rt_view();
    // A frame with no previous camera copies the current one: the pass computes no motion.
    frame.previous_eye = frame.eye;
    frame.previous_forward = frame.forward;
    frame.previous_up = frame.up;
    const std::vector<std::uint8_t> still = pack_frame_parameters(frame, kRtSize, kRtSize);
    REQUIRE(still.size() == kRtParameterBytes);
    CHECK(read_float(still, 144) == frame.eye.x);
    CHECK(read_float(still, 160) == frame.forward.x);
    CHECK(read_float(still, 168) == frame.forward.z);
    CHECK(read_float(still, 176) == read_float(still, 16));
    CHECK(read_float(still, 192) == read_float(still, 32));

    // A previous camera that moved right and turned: its axes are scaled like the current ones, so the shader projects with one formula.
    frame.previous_eye = glm::vec3(0.5f, 0.0f, 0.25f);
    frame.previous_forward = glm::normalize(glm::vec3(0.1f, 0.0f, -1.0f));
    frame.previous_up = glm::vec3(0.0f, 1.0f, 0.0f);
    const std::vector<std::uint8_t> moved = pack_frame_parameters(frame, kRtSize, kRtSize);
    CHECK(read_float(moved, 144) == 0.5f);
    CHECK(read_float(moved, 152) == 0.25f);
    CHECK(read_float(moved, 160) == Catch::Approx(frame.previous_forward.x));
    CHECK(read_float(moved, 168) == Catch::Approx(frame.previous_forward.z));
    const float tan_half = std::tan(0.5f * frame.fov_y_radians);
    const glm::vec3 previous_right = glm::normalize(glm::cross(frame.previous_forward, frame.previous_up));
    CHECK(read_float(moved, 176) == Catch::Approx(previous_right.x * tan_half * frame.aspect).margin(1.0e-6));
    CHECK(read_float(moved, 192 + 4) == Catch::Approx(tan_half));

    // A previous forward that is not a unit vector is refused before any pass runs.
    frame.previous_forward = glm::vec3(0.0f, 0.0f, -2.0f);
    CHECK(validate_frame(frame).has_value());
}
