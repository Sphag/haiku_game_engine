// Model import (US-020) and cooking (US-023): the model payload format, the model importer's settings, and the glTF/GLB,
// FBX, and OBJ fixtures. Each fixture is one triangle (tests/fixtures/models/generate_fixtures.py), so the expected values
// below are worked out by hand. Payloads are read back with read_model_payload, which applies the same checks the converter
// relies on. Cooking reorders vertices, so the import tests find a vertex by its position, not by its index.

#include "asset_converter/importer.hpp"
#include "asset_converter/mesh_cooking.hpp"
#include "asset_converter/model_format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using engine::converter::AlphaMode;
using engine::converter::ImportOutput;
using engine::converter::ModelMaterial;
using engine::converter::ModelMesh;
using engine::converter::ModelNode;
using engine::converter::ModelPayload;
using engine::converter::read_model_payload;
using engine::converter::validate_model;
using engine::converter::write_model_payload;
namespace fs = std::filesystem;

namespace {

constexpr float kEpsilon = 1e-5f;

bool near(float a, float b) {
    return std::fabs(a - b) <= kEpsilon;
}

bool near(const glm::vec3& a, const glm::vec3& b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

bool near(const glm::vec2& a, const glm::vec2& b) {
    return near(a.x, b.x) && near(a.y, b.y);
}

std::string read_bytes(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::ostringstream bytes;
    bytes << stream.rdbuf();
    return bytes.str();
}

fs::path model_fixture(const char* name) {
    return fs::path(std::string(ENGINE_TEST_FIXTURE_DIR)) / "models" / name;
}

// Imports a source with the model importer, the way the converter does: canonical settings first, then the import.
ImportOutput import_model(const std::string& source, const std::string& settings_json = "{}") {
    const auto importer = engine::converter::make_model_importer();
    auto canonical = importer->canonical_settings(settings_json);
    INFO("settings rejected: " << (canonical.ok() ? "" : canonical.error()));
    REQUIRE(canonical.ok());
    auto imported = importer->import(source, canonical.value());
    INFO("import failed: " << (imported.ok() ? "" : imported.error()));
    REQUIRE(imported.ok());
    return std::move(imported).value();
}

ModelPayload read_payload(const std::string& bytes) {
    auto model = read_model_payload(bytes);
    INFO("payload rejected: " << (model.ok() ? "" : model.error()));
    REQUIRE(model.ok());
    return std::move(model).value();
}

// The transform from the root down to a node: the product of the locals along the parent chain, root first.
glm::mat4 world_of(const ModelPayload& model, std::int32_t node) {
    glm::mat4 world(1.0f);
    for (; node >= 0; node = model.nodes[static_cast<std::size_t>(node)].parent) {
        world = model.nodes[static_cast<std::size_t>(node)].local * world;
    }
    return world;
}

// Positions of a mesh in world space, through the first node that references it.
std::vector<glm::vec3> world_positions(const ModelPayload& model, std::uint32_t mesh) {
    for (std::size_t n = 0; n < model.nodes.size(); ++n) {
        const auto& meshes = model.nodes[n].meshes;
        if (std::find(meshes.begin(), meshes.end(), mesh) != meshes.end()) {
            const glm::mat4 world = world_of(model, static_cast<std::int32_t>(n));
            std::vector<glm::vec3> positions;
            for (const glm::vec3& position : model.meshes[mesh].positions) {
                positions.push_back(glm::vec3(world * glm::vec4(position, 1.0f)));
            }
            return positions;
        }
    }
    INFO("no node references mesh " << mesh);
    CHECK(false);
    return {};
}

// The index of the vertex at `expected`, or the vertex count when no vertex is there.
std::size_t find_vertex(const std::vector<glm::vec3>& positions, const glm::vec3& expected) {
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (near(positions[i], expected)) {
            return i;
        }
    }
    INFO("no vertex at " << expected.x << ", " << expected.y << ", " << expected.z);
    CHECK(false);
    return positions.size();
}

bool mentions(const std::vector<std::string>& notes, const std::string& text) {
    return std::any_of(notes.begin(), notes.end(),
                       [&text](const std::string& note) { return note.find(text) != std::string::npos; });
}

// Cooks a mesh the way the importer does, and requires the cooking to succeed.
void cook(ModelMesh& mesh, std::vector<std::uint32_t> triangles) {
    auto cooked = engine::converter::cook_mesh(mesh, std::move(triangles));
    INFO("cook failed: " << (cooked.ok() ? "" : cooked.error()));
    REQUIRE(cooked.ok());
}

// One mesh with every attribute, one material with textures, and a three-node chain, for the format tests.
ModelPayload sample_model() {
    ModelPayload model;
    ModelMesh mesh;
    mesh.name = "Triangle \xD1\x82";  // the name ends in a Cyrillic letter, so the UTF-8 check is exercised
    mesh.material = 0;
    mesh.positions = {glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
    mesh.normals = {glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f)};
    mesh.tangents = {glm::vec4(1.0f, 0.0f, 0.0f, -1.0f), glm::vec4(1.0f, 0.0f, 0.0f, -1.0f), glm::vec4(1.0f, 0.0f, 0.0f, -1.0f)};
    mesh.uvs = {glm::vec2(0.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 0.0f)};
    cook(mesh, {0, 1, 2});
    model.meshes.push_back(mesh);

    ModelMesh bare;
    bare.name = "";
    bare.material = -1;
    bare.positions = {glm::vec3(2.0f), glm::vec3(3.0f, 2.0f, 2.0f), glm::vec3(2.0f, 3.0f, 2.0f)};
    cook(bare, {2, 1, 0});
    model.meshes.push_back(bare);

    ModelMaterial material;
    material.name = "Paint";
    material.base_color = glm::vec4(0.8f, 0.2f, 0.1f, 0.5f);
    material.metallic = 0.25f;
    material.roughness = 0.5f;
    material.emissive = glm::vec3(0.1f, 0.0f, 0.0f);
    material.alpha_mode = AlphaMode::Blend;
    material.double_sided = true;
    material.base_color_texture = "textures/paint.png";
    model.materials.push_back(material);

    model.nodes.push_back(ModelNode{"Root", -1, glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)), {}});
    model.nodes.push_back(ModelNode{"Child", 0, glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)), {0, 1}});
    model.nodes.push_back(ModelNode{"Leaf", 1, glm::mat4(1.0f), {}});
    return model;
}

}  // namespace

TEST_CASE("Model payload: every attribute survives a write and read, and corrupt bytes are rejected", "[unit]") {
    const ModelPayload model = sample_model();
    REQUIRE(validate_model(model).ok());
    const std::string bytes = write_model_payload(model);
    CHECK(write_model_payload(model) == bytes);  // identical models give identical bytes

    const ModelPayload back = read_payload(bytes);
    REQUIRE(back.meshes.size() == 2);
    CHECK(back.meshes[0].name == model.meshes[0].name);
    CHECK(back.meshes[0].material == 0);
    CHECK(back.meshes[0].lods.size() == model.meshes[0].lods.size());
    CHECK(back.meshes[0].lods[0].indices == model.meshes[0].lods[0].indices);
    CHECK(back.meshes[0].lods[0].meshlets.size() == model.meshes[0].lods[0].meshlets.size());
    CHECK(back.meshes[0].bounds.radius == model.meshes[0].bounds.radius);
    REQUIRE(back.meshes[0].tangents.size() == 3);
    CHECK(back.meshes[0].tangents == model.meshes[0].tangents);
    for (const glm::vec4& tangent : back.meshes[0].tangents) {
        CHECK(near(tangent.w, -1.0f));
    }
    CHECK(back.meshes[0].uvs == model.meshes[0].uvs);
    CHECK(back.meshes[1].normals.empty());
    CHECK(back.meshes[1].uvs.empty());
    CHECK(back.meshes[1].material == -1);
    REQUIRE(back.materials.size() == 1);
    CHECK(back.materials[0].alpha_mode == AlphaMode::Blend);
    CHECK(back.materials[0].double_sided);
    CHECK(back.materials[0].base_color_texture == "textures/paint.png");
    CHECK(back.materials[0].normal_texture.empty());
    REQUIRE(back.nodes.size() == 3);
    CHECK(back.nodes[1].parent == 0);
    CHECK(back.nodes[1].meshes == std::vector<std::uint32_t>{0, 1});
    CHECK(near(back.nodes[1].local[0][0], 2.0f));

    CHECK(!read_model_payload("XXXX" + bytes.substr(4)).ok());        // bad magic
    CHECK(!read_model_payload(bytes.substr(0, bytes.size() - 1)).ok());  // truncated
    CHECK(!read_model_payload(bytes + std::string(1, '\0')).ok());     // trailing byte
    CHECK(!read_model_payload(bytes.substr(0, 8)).ok());              // header only
    CHECK(!read_model_payload("").ok());

    std::string huge_count = bytes;
    huge_count.replace(8, 4, std::string("\xff\xff\xff\xff", 4));    // mesh count larger than the file
    CHECK(!read_model_payload(huge_count).ok());

    std::string bad_version = bytes;
    bad_version[4] = 1;  // the format before cooking (version 1) is not read
    CHECK(!read_model_payload(bad_version).ok());
    bad_version[4] = 3;
    CHECK(!read_model_payload(bad_version).ok());
}

TEST_CASE("Model payload: the validation rules reject references and numbers that would crash a reader", "[unit]") {
    ModelPayload index_out = sample_model();
    index_out.meshes[0].lods[0].indices[2] = 9;
    CHECK(!validate_model(index_out).ok());

    ModelPayload triangle_count = sample_model();
    triangle_count.meshes[0].lods[0].indices.pop_back();
    CHECK(!validate_model(triangle_count).ok());

    ModelPayload short_normals = sample_model();
    short_normals.meshes[0].normals.pop_back();
    CHECK(!validate_model(short_normals).ok());

    ModelPayload bad_material = sample_model();
    bad_material.meshes[1].material = 4;
    CHECK(!validate_model(bad_material).ok());

    ModelPayload parent_after = sample_model();
    parent_after.nodes[0].parent = 2;
    CHECK(!validate_model(parent_after).ok());

    ModelPayload nan_position = sample_model();
    nan_position.meshes[0].positions[1].x = std::numeric_limits<float>::quiet_NaN();
    CHECK(!validate_model(nan_position).ok());

    ModelPayload nan_transform = sample_model();
    nan_transform.nodes[2].local[3][0] = std::numeric_limits<float>::infinity();
    CHECK(!validate_model(nan_transform).ok());

    ModelPayload unknown_alpha = sample_model();
    unknown_alpha.materials[0].alpha_mode = static_cast<AlphaMode>(7);
    CHECK(!validate_model(unknown_alpha).ok());

    ModelPayload bad_mesh_ref = sample_model();
    bad_mesh_ref.nodes[2].meshes.push_back(5);
    CHECK(!validate_model(bad_mesh_ref).ok());
}

TEST_CASE("Model importer: settings canonicalize, and a scale that is not positive and finite is rejected", "[unit]") {
    const auto importer = engine::converter::make_model_importer();
    CHECK(importer->type() == "model");
    CHECK(importer->canonical_settings("{}").value() == "{\"scale\":1.0}");
    CHECK(importer->canonical_settings("{\"scale\":2}").value() == "{\"scale\":2.0}");
    CHECK(importer->canonical_settings("{\"scale\":2.0}").value() == "{\"scale\":2.0}");
    CHECK(!importer->canonical_settings("{\"scale\":0}").ok());
    CHECK(!importer->canonical_settings("{\"scale\":-1}").ok());
    CHECK(!importer->canonical_settings("{\"scale\":\"2\"}").ok());
    CHECK(!importer->canonical_settings("{\"bogus\":1}").ok());
    CHECK(!importer->canonical_settings("[]").ok());
    CHECK(!importer->canonical_settings("not json").ok());
}

TEST_CASE("Model import: glTF and GLB fixtures give the same geometry, transforms, and material", "[integration]") {
    const ImportOutput gltf = import_model(read_bytes(model_fixture("triangle_tree.gltf")));
    const ImportOutput glb = import_model(read_bytes(model_fixture("triangle_tree.glb")));
    CHECK(gltf.payload == glb.payload);
    CHECK(import_model(read_bytes(model_fixture("triangle_tree.gltf"))).payload == gltf.payload);  // deterministic

    const ModelPayload model = read_payload(gltf.payload);
    REQUIRE(model.meshes.size() == 1);
    const ModelMesh& mesh = model.meshes[0];
    CHECK(mesh.lods[0].indices.size() == 3);

    // Holder translates by (10,0,0); Tri translates by (0,2,0) and scales by 2.
    const std::vector<glm::vec3> world = world_positions(model, 0);
    REQUIRE(world.size() == 3);
    const std::size_t origin = find_vertex(world, glm::vec3(10.0f, 2.0f, 0.0f));
    const std::size_t right = find_vertex(world, glm::vec3(12.0f, 2.0f, 0.0f));
    const std::size_t top = find_vertex(world, glm::vec3(10.0f, 4.0f, 0.0f));
    REQUIRE(origin < world.size());
    REQUIRE(right < world.size());
    REQUIRE(top < world.size());

    REQUIRE(mesh.normals.size() == 3);
    for (const glm::vec3& normal : mesh.normals) {
        CHECK(near(normal, glm::vec3(0.0f, 0.0f, 1.0f)));
    }
    REQUIRE(mesh.uvs.size() == 3);
    CHECK(near(mesh.uvs[origin], glm::vec2(0.0f, 0.0f)));  // glTF UVs keep their own orientation
    CHECK(near(mesh.uvs[top], glm::vec2(0.0f, 1.0f)));
    REQUIRE(mesh.tangents.size() == 3);
    for (const glm::vec4& tangent : mesh.tangents) {
        CHECK(near(tangent.x, 1.0f));
        CHECK(near(tangent.w, 1.0f));  // the file's tangent is kept, with its own handedness
    }

    // Assimp appends its own default material after the file's materials, so the file's material is index 0 and is
    // the one the mesh uses.
    REQUIRE(model.materials.size() >= 1);
    const ModelMaterial& material = model.materials[0];
    CHECK(material.name == "Paint");
    CHECK(near(material.base_color.r, 0.8f));
    CHECK(near(material.base_color.g, 0.2f));
    CHECK(near(material.base_color.b, 0.1f));
    CHECK(near(material.base_color.a, 1.0f));
    CHECK(near(material.metallic, 0.25f));
    CHECK(near(material.roughness, 0.5f));
    CHECK(material.double_sided);
    CHECK(material.alpha_mode == AlphaMode::Opaque);
    CHECK(material.base_color_texture.empty());
    CHECK(mesh.material == 0);

    // The vertex color attribute is not imported, and the import says so.
    CHECK(mentions(gltf.unsupported, "vertex colors"));
    CHECK(mentions(glb.unsupported, "vertex colors"));
}

TEST_CASE("Model import: FBX in centimeters becomes meters in a Y-up frame", "[integration]") {
    const ImportOutput fbx = import_model(read_bytes(model_fixture("fbx_cm_yup.fbx")));
    const ModelPayload model = read_payload(fbx.payload);
    REQUIRE(model.meshes.size() == 1);
    const ModelMesh& mesh = model.meshes[0];

    // The triangle is 100 units (1 meter) on each side, and its model translates by 1000 units (10 meters).
    const std::vector<glm::vec3> world = world_positions(model, 0);
    REQUIRE(world.size() == 3);
    const std::size_t origin = find_vertex(world, glm::vec3(10.0f, 0.0f, 0.0f));
    const std::size_t top = find_vertex(world, glm::vec3(10.0f, 1.0f, 0.0f));
    REQUIRE(origin < world.size());
    REQUIRE(top < world.size());
    CHECK(find_vertex(world, glm::vec3(11.0f, 0.0f, 0.0f)) < world.size());

    // FBX stores v upward, so v is flipped to the engine's top-row origin. The generated tangent points along +u with
    // handedness -1, because the flip reverses the bitangent.
    REQUIRE(mesh.uvs.size() == 3);
    CHECK(near(mesh.uvs[origin], glm::vec2(0.0f, 1.0f)));
    CHECK(near(mesh.uvs[top], glm::vec2(0.0f, 0.0f)));
    REQUIRE(mesh.tangents.size() == 3);
    for (const glm::vec4& tangent : mesh.tangents) {
        CHECK(near(tangent.x, 1.0f));
        CHECK(near(tangent.w, -1.0f));
    }
    CHECK(mesh.normals.size() == 3);
}

TEST_CASE("Model import: FBX in meters with Z-up is converted to Y-up", "[integration]") {
    const ImportOutput fbx = import_model(read_bytes(model_fixture("fbx_m_zup.fbx")));
    const ModelPayload model = read_payload(fbx.payload);
    REQUIRE(model.meshes.size() == 1);

    // Right-handed Z-up becomes Y-up: (x, y, z) maps to (x, z, -y). The model sits 2 meters up, so its height is Y = 2.
    const std::vector<glm::vec3> world = world_positions(model, 0);
    REQUIRE(world.size() == 3);
    CHECK(find_vertex(world, glm::vec3(0.0f, 2.0f, 0.0f)) < world.size());
    CHECK(find_vertex(world, glm::vec3(1.0f, 2.0f, 0.0f)) < world.size());
    CHECK(find_vertex(world, glm::vec3(0.0f, 2.0f, -1.0f)) < world.size());
}

TEST_CASE("Model import: the OBJ fixture has flipped UVs, computed tangents, and one note for its missing library",
          "[integration]") {
    const ImportOutput obj = import_model(read_bytes(model_fixture("triangle.obj")));
    const ModelPayload model = read_payload(obj.payload);
    REQUIRE(model.meshes.size() == 1);
    const ModelMesh& mesh = model.meshes[0];

    const std::vector<glm::vec3> world = world_positions(model, 0);
    REQUIRE(world.size() == 3);
    const std::size_t origin = find_vertex(world, glm::vec3(0.0f));
    const std::size_t top = find_vertex(world, glm::vec3(0.0f, 1.0f, 0.0f));
    REQUIRE(origin < world.size());
    REQUIRE(top < world.size());
    CHECK(find_vertex(world, glm::vec3(1.0f, 0.0f, 0.0f)) < world.size());
    REQUIRE(mesh.uvs.size() == 3);
    CHECK(near(mesh.uvs[origin], glm::vec2(0.0f, 1.0f)));
    CHECK(near(mesh.uvs[top], glm::vec2(0.0f, 0.0f)));
    REQUIRE(mesh.tangents.size() == 3);
    for (const glm::vec4& tangent : mesh.tangents) {
        CHECK(near(tangent.w, -1.0f));
    }

    // The material library is named twice in Assimp's lookups; the import reports the file once.
    const auto missing = std::count_if(obj.unsupported.begin(), obj.unsupported.end(),
                                       [](const std::string& note) { return note.find("missing.mtl") != std::string::npos; });
    CHECK(missing == 1);
    CHECK(mesh.material >= 0);
    CHECK(static_cast<std::size_t>(mesh.material) < model.materials.size());
}

TEST_CASE("Model import: the scale setting multiplies the whole model", "[integration]") {
    const ImportOutput scaled = import_model(read_bytes(model_fixture("triangle_tree.gltf")), "{\"scale\":0.5}");
    const ModelPayload model = read_payload(scaled.payload);
    const std::vector<glm::vec3> world = world_positions(model, 0);
    REQUIRE(world.size() == 3);
    CHECK(find_vertex(world, glm::vec3(5.0f, 1.0f, 0.0f)) < world.size());
    CHECK(find_vertex(world, glm::vec3(6.0f, 1.0f, 0.0f)) < world.size());
    CHECK(find_vertex(world, glm::vec3(5.0f, 2.0f, 0.0f)) < world.size());
    // Normals are directions and are not scaled.
    for (const glm::vec3& normal : model.meshes[0].normals) {
        CHECK(near(normal, glm::vec3(0.0f, 0.0f, 1.0f)));
    }
}

TEST_CASE("Model import: missing normals, UVs, materials, and line data are handled without failing", "[integration]") {
    // A glTF triangle with no NORMAL, TANGENT, or TEXCOORD attributes, no material on the primitive, and an empty material.
    const std::string gltf =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"materials":[{}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
        R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],)"
        R"("buffers":[{"byteLength":42,"uri":)"
        R"("data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}]})";
    const ModelPayload gltf_model = read_payload(import_model(gltf).payload);
    REQUIRE(gltf_model.meshes.size() == 1);
    CHECK(gltf_model.meshes[0].uvs.empty());       // no UVs were provided, so none are invented
    CHECK(gltf_model.meshes[0].tangents.empty());  // and no tangents without UVs
    CHECK(gltf_model.meshes[0].normals.size() == 3);  // normals are generated when the source has none
    CHECK(gltf_model.meshes[0].lods[0].indices.size() == 3);

    // An OBJ with no normals or UVs, a line element, and no material library.
    const ImportOutput obj = import_model("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nl 1 2\n");
    const ModelPayload obj_model = read_payload(obj.payload);
    REQUIRE(obj_model.meshes.size() >= 1);
    CHECK(obj_model.meshes[0].lods[0].indices.size() == 3);
    CHECK(obj_model.meshes[0].tangents.empty());
    CHECK(mentions(obj.unsupported, "line"));
}

TEST_CASE("Model import: a source that is not a model fails with a message", "[integration]") {
    const auto importer = engine::converter::make_model_importer();
    const std::string settings = importer->canonical_settings("{}").value();
    const auto garbage = importer->import("this is plain text, not a model\n", settings);
    CHECK(!garbage.ok());
    const auto truncated = importer->import(read_bytes(model_fixture("triangle_tree.glb")).substr(0, 40), settings);
    CHECK(!truncated.ok());
}
