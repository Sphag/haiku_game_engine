// CPU checks of the physically based material model (US-034): color spaces, equirect mapping, image-based lighting, the shading reference,
// the blend-mode output rules, material validation, and mesh orientation. The GPU suites compare against these same functions.

#include "render_material_fixture.hpp"

#include "engine/render/material.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/pbr.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using engine::render::BlendMode;
using engine::render::Environment;
using engine::render::EquirectImage;
using engine::render::ImageRgba8;
using engine::render::MaterialImages;
using engine::render::MaterialOverrides;
using engine::render::MaterialParams;
using engine::render::MeshData;
using engine::render::ShadedSample;
using engine::render::SurfaceInput;
using engine::render::tests::solid_image;
using engine::render::tests::uniform_radiance;
using Catch::Matchers::WithinAbs;

namespace {

// The unit tests use a plain front-lit surface: normal and view along +Z, tangent along +X, bitangent along -Y (the plane's layout).
SurfaceInput facing_input() {
    SurfaceInput input;
    input.normal = glm::vec3(0.0f, 0.0f, 1.0f);
    input.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
    input.bitangent = glm::vec3(0.0f, -1.0f, 0.0f);
    input.view = glm::vec3(0.0f, 0.0f, 1.0f);
    return input;
}

// The triangle normal from the winding, (b - a) x (c - a), summed over a mesh's triangles and compared with the outward direction at each.
bool windings_point_outward(const MeshData& mesh, const glm::vec3& center) {
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const glm::vec3& a = mesh.positions[mesh.indices[t]];
        const glm::vec3& b = mesh.positions[mesh.indices[t + 1]];
        const glm::vec3& c = mesh.positions[mesh.indices[t + 2]];
        const glm::vec3 face = glm::cross(b - a, c - a);
        const glm::vec3 centroid = (a + b + c) / 3.0f;
        if (glm::length(face) > 0.0f && glm::dot(face, centroid - center) <= 0.0f) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("sRGB decoding and encoding round-trip every byte, and hit the transfer function's known points", "[unit]") {
    CHECK_THAT(static_cast<double>(engine::render::srgb_to_linear(0.0f)), WithinAbs(0.0, 1e-7));
    CHECK_THAT(static_cast<double>(engine::render::srgb_to_linear(1.0f)), WithinAbs(1.0, 1e-6));
    // Mid gray: sRGB 0.5 is about 0.214 linear, and linear 0.5 encodes to byte 188.
    CHECK_THAT(static_cast<double>(engine::render::srgb_to_linear(0.5f)), WithinAbs(0.2140, 1e-3));
    CHECK(engine::render::encode_srgb8(0.5f) == 188);
    CHECK(engine::render::encode_srgb8(-1.0f) == 0);
    CHECK(engine::render::encode_srgb8(2.0f) == 255);
    for (int byte = 0; byte <= 255; ++byte) {
        const float linear = engine::render::srgb_to_linear(static_cast<float>(byte) / 255.0f);
        CHECK(engine::render::encode_srgb8(linear) == static_cast<std::uint8_t>(byte));
    }
}

TEST_CASE("Equirect coordinates map each direction to one place and back, and the poles and the seam are where they belong", "[unit]") {
    const glm::vec3 directions[] = {glm::normalize(glm::vec3(0.3f, -0.4f, 0.8f)), glm::normalize(glm::vec3(-1.0f, 0.2f, -0.1f)),
                                    glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)};
    for (const glm::vec3& d : directions) {
        const glm::vec3 back = engine::render::equirect_to_direction(engine::render::direction_to_equirect(d));
        CHECK_THAT(static_cast<double>(glm::length(back - d)), WithinAbs(0.0, 1e-5));
    }
    CHECK_THAT(static_cast<double>(engine::render::direction_to_equirect(glm::vec3(0.0f, 1.0f, 0.0f)).y), WithinAbs(0.0, 1e-6));
    CHECK_THAT(static_cast<double>(engine::render::direction_to_equirect(glm::vec3(1.0f, 0.0f, 0.0f)).x), WithinAbs(0.5, 1e-6));
}

TEST_CASE("Image-based lighting of a uniform environment is that radiance in every direction, at every roughness", "[unit]") {
    const Environment environment = engine::render::build_environment(uniform_radiance(0.25f));
    // Irradiance over pi is the radiance for a white diffuse surface, so a uniform environment gives it back.
    const glm::vec3 normals[] = {glm::vec3(0.0f, 0.0f, 1.0f), glm::normalize(glm::vec3(0.4f, 0.7f, -0.2f)), glm::vec3(1.0f, 0.0f, 0.0f)};
    for (const glm::vec3& n : normals) {
        CHECK_THAT(static_cast<double>(engine::render::evaluate_diffuse(environment, n).x), WithinAbs(0.25, 0.01));
    }
    // Every prefiltered level holds the same radiance, so the specular term is the same at every roughness.
    for (const float roughness : {0.0f, 0.3f, 0.7f, 1.0f}) {
        const glm::vec3 specular = engine::render::sample_specular(environment, glm::vec3(0.0f, 0.0f, 1.0f), roughness);
        CHECK_THAT(static_cast<double>(specular.x), WithinAbs(0.25, 1e-4));
    }
}

TEST_CASE("The BRDF table is bounded, and a head-on smooth view reflects everything", "[unit]") {
    const Environment environment = engine::render::build_environment(uniform_radiance(1.0f));
    for (const glm::vec2& entry : environment.brdf_lut) {
        CHECK(entry.x >= 0.0f);
        CHECK(entry.y >= 0.0f);
        CHECK(entry.x + entry.y <= 1.0f + 0.02f);  // importance sampling leaves a little noise
    }
    // At a head-on view and zero roughness, the scale is one and the bias is zero: the surface reflects its full Fresnel term.
    const glm::vec2 smooth = engine::render::sample_brdf_lut(environment, 0.99f, 0.0f);
    CHECK_THAT(static_cast<double>(smooth.x), WithinAbs(1.0, 0.05));
    CHECK_THAT(static_cast<double>(smooth.y), WithinAbs(0.0, 0.05));
}

TEST_CASE("A white dielectric under a uniform environment returns the light it gets at every roughness, so the split sum conserves energy", "[unit]") {
    const Environment environment = engine::render::build_environment(uniform_radiance(1.0f));
    for (const float roughness : {0.0f, 0.5f, 1.0f}) {
        MaterialParams params;
        params.base_color = glm::vec4(1.0f);
        params.metallic = 0.0f;
        params.roughness = roughness;
        const ShadedSample sample = engine::render::shade_reference(facing_input(), params, {}, environment);
        CHECK_THAT(static_cast<double>(sample.radiance.r), WithinAbs(1.0, 0.03));
    }
}

TEST_CASE("A metal reflects its color through the Fresnel term, and a rough black surface returns only its emissive light", "[unit]") {
    const Environment environment = engine::render::build_environment(uniform_radiance(1.0f));
    MaterialParams metal;
    metal.base_color = glm::vec4(0.5f, 0.25f, 1.0f, 1.0f);
    metal.metallic = 1.0f;
    metal.roughness = 0.0f;
    const ShadedSample reflected = engine::render::shade_reference(facing_input(), metal, {}, environment);
    CHECK_THAT(static_cast<double>(reflected.radiance.g), WithinAbs(0.25, 0.02));
    CHECK_THAT(static_cast<double>(reflected.radiance.b), WithinAbs(1.0, 0.03));

    MaterialParams glow;
    glow.base_color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    glow.roughness = 1.0f;
    glow.emissive = glm::vec3(0.5f, 0.0f, 0.0f);
    const ShadedSample lit = engine::render::shade_reference(facing_input(), glow, {}, environment);
    CHECK_THAT(static_cast<double>(lit.radiance.r), WithinAbs(0.5 + 0.0, 0.1));
    CHECK_THAT(static_cast<double>(lit.radiance.g), WithinAbs(0.0, 0.05));
}

TEST_CASE("Normal maps tilt the normal in the tangent frame, and a flat map leaves it alone", "[unit]") {
    // The sun-and-sky environment is not uniform, so a tilted normal sees different light from a flat one.
    const Environment environment = engine::render::build_environment(engine::render::tests::sun_sky());
    ImageRgba8 tilt = solid_image(1, 1, 255, 128, 128, 255);  // tangent-space normal near +X
    MaterialImages images;
    images.normal = &tilt;
    MaterialParams params;
    params.roughness = 0.4f;
    params.base_color = glm::vec4(1.0f);
    // The tangent +X becomes the normal, so the view at +Z sees a surface turned away from the light's average direction. The result must differ
    // from the flat map and stay in range.
    const ShadedSample tilted = engine::render::shade_reference(facing_input(), params, images, environment);
    const ShadedSample flat = engine::render::shade_reference(facing_input(), params, {}, environment);
    CHECK(glm::length(tilted.radiance - flat.radiance) > 0.01f);
    // A zero normal scale removes the tilt, whatever the texels say. The map's own flat-normal offset (about 0.4 degrees) remains, so the
    // result is close to the flat map's, not identical.
    params.normal_scale = 0.0f;
    const ShadedSample unscaled = engine::render::shade_reference(facing_input(), params, images, environment);
    CHECK_THAT(static_cast<double>(unscaled.radiance.r), WithinAbs(static_cast<double>(flat.radiance.r), 1e-2));
}

TEST_CASE("Blend modes decide the output: opaque forces alpha one, masks discard below the cutoff, and blends keep the coverage", "[unit]") {
    ShadedSample sample;
    sample.radiance = glm::vec3(0.5f);
    sample.alpha = 0.25f;

    MaterialParams opaque;
    const auto opaque_pixel = engine::render::to_pixel(sample, opaque);
    CHECK_FALSE(opaque_pixel.discarded);
    CHECK(opaque_pixel.rgba[3] == 255);

    MaterialParams mask;
    mask.blend = BlendMode::AlphaMask;
    mask.alpha_cutoff = 0.5f;
    CHECK(engine::render::to_pixel(sample, mask).discarded);
    sample.alpha = 0.75f;
    const auto kept = engine::render::to_pixel(sample, mask);
    CHECK_FALSE(kept.discarded);
    CHECK(kept.rgba[3] == 255);

    MaterialParams blend;
    blend.blend = BlendMode::AlphaBlend;
    const auto translucent = engine::render::to_pixel(sample, blend);
    CHECK(translucent.rgba[3] == 191);  // 0.75 * 255 rounded
}

TEST_CASE("Material validation rejects out-of-range and non-finite parameters, and instance overrides keep the parent's other values", "[unit]") {
    CHECK_FALSE(engine::render::validate_material(MaterialParams{}).has_value());

    MaterialParams roughness;
    roughness.roughness = 1.5f;
    CHECK(engine::render::validate_material(roughness).has_value());

    MaterialParams metallic;
    metallic.metallic = std::numeric_limits<float>::quiet_NaN();
    CHECK(engine::render::validate_material(metallic).has_value());

    MaterialParams emissive;
    emissive.emissive = glm::vec3(-1.0f, 0.0f, 0.0f);
    CHECK(engine::render::validate_material(emissive).has_value());

    MaterialParams parent;
    parent.roughness = 0.2f;
    parent.two_sided = true;
    MaterialOverrides overrides;
    overrides.metallic = 0.75f;
    const MaterialParams instance = engine::render::resolve(parent, overrides);
    CHECK(instance.metallic == 0.75f);
    CHECK(instance.roughness == 0.2f);
    CHECK(instance.two_sided);
    CHECK(engine::render::resolve(parent, MaterialOverrides{}).roughness == 0.2f);

    ImageRgba8 image = solid_image(2, 2, 0, 0, 0, 255);
    CHECK_FALSE(engine::render::validate_image(image).has_value());
    image.pixels.pop_back();
    CHECK(engine::render::validate_image(image).has_value());
    ImageRgba8 empty;
    CHECK(engine::render::validate_image(empty).has_value());
}

TEST_CASE("Radiance images validate their size and reject negative or non-finite texels", "[unit]") {
    CHECK_FALSE(engine::render::validate_equirect(uniform_radiance(1.0f)).has_value());
    EquirectImage negative = uniform_radiance(1.0f);
    negative.texels[3].g = -0.5f;
    CHECK(engine::render::validate_equirect(negative).has_value());
    EquirectImage short_image = uniform_radiance(1.0f);
    short_image.texels.pop_back();
    CHECK(engine::render::validate_equirect(short_image).has_value());
}

TEST_CASE("Mesh normals and UVs validate, and derived normals point away from the surface", "[unit]") {
    MeshData sphere = engine::render::make_sphere_mesh(2.0f, 8, 12);
    CHECK_FALSE(engine::render::validate_mesh(sphere).has_value());
    CHECK(windings_point_outward(sphere, glm::vec3(0.0f)));
    for (std::size_t i = 0; i < sphere.positions.size(); ++i) {
        CHECK_THAT(static_cast<double>(glm::length(sphere.normals[i])), WithinAbs(1.0, 1e-5));
    }

    const MeshData box = engine::render::make_box_mesh(glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(box.positions.size() == 24);
    CHECK(box.indices.size() == 36);
    CHECK(windings_point_outward(box, glm::vec3(0.0f)));

    // A plane has one +Z normal, and its triangles' normals point along +Z.
    const MeshData plane = engine::render::make_plane_mesh(1.0f);
    CHECK(windings_point_outward(plane, glm::vec3(0.0f, 0.0f, -10.0f)));

    // Without stored normals, the normals come from the windings: a closed sphere's derived normals point out.
    MeshData bare = sphere;
    bare.normals.clear();
    const std::vector<glm::vec3> derived = engine::render::vertex_normals(bare);
    for (std::size_t i = 0; i < derived.size(); ++i) {
        CHECK(glm::dot(derived[i], glm::normalize(sphere.positions[i])) > 0.9f);
    }

    MeshData mismatched = box;
    mismatched.uvs.pop_back();
    CHECK(engine::render::validate_mesh(mismatched).has_value());
    MeshData bad_normal = box;
    bad_normal.normals[0] = glm::vec3(0.0f, 0.0f, 2.0f);
    CHECK(engine::render::validate_mesh(bad_normal).has_value());
}
