#include "asset_converter/model_format.hpp"

#include "engine/core/utf8.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>

namespace engine::converter {
namespace {

using core::Result;
using core::Unit;
using core::failure;
using core::success;

constexpr std::uint32_t kFlagNormals = 1u << 0;
constexpr std::uint32_t kFlagTangents = 1u << 1;
constexpr std::uint32_t kFlagUvs = 1u << 2;
constexpr std::uint32_t kFlagSkin = 1u << 3;
constexpr std::uint32_t kKnownFlags = kFlagNormals | kFlagTangents | kFlagUvs | kFlagSkin;
constexpr std::uint32_t kStaticVersion = 2;  // a model with no joints or clips is written in the version that predates them
constexpr std::uint32_t kLastAlphaMode = static_cast<std::uint32_t>(AlphaMode::Blend);
// Relative slack for bounds checks. The bounds come from float arithmetic, so an exact sphere test would reject
// correct output.
constexpr float kBoundsTolerance = 1e-4f;

using Triangle = std::array<std::uint32_t, 3>;

// A triangle rotated so its smallest vertex index comes first. Rotation keeps the winding, so two copies of a triangle
// that start at different corners compare equal.
Triangle canonical_triangle(Triangle triangle) {
    const auto smallest = std::min_element(triangle.begin(), triangle.end());
    std::rotate(triangle.begin(), smallest, triangle.end());
    return triangle;
}

// Smallest number of bytes one element of each array can occupy. The reader multiplies a count by these before it
// allocates, so a count larger than the file can hold is rejected first.
constexpr std::size_t kMeshMinBytes = 4 + 4 + 4 + 4 + 10 * 4 + 4;  // name, material, vertex count, flags, bounds, LOD count
constexpr std::size_t kLodMinBytes = 4 + 4 + 4 + 4 + 4;  // error, index count, meshlet count, meshlet vertex and triangle counts
constexpr std::size_t kMeshletBytes = 4 * 4 + 3 * 4 + 4 + 3 * 4 + 4;  // offsets and counts, sphere, cone (48 bytes)
constexpr std::size_t kMaterialMinBytes = 4 + 16 + 4 + 4 + 12 + 4 + 4 + 1 + 4 * 4;
constexpr std::size_t kNodeMinBytes = 4 + 4 + 64 + 4;

struct ReadError {
    std::string message;
};

// The bits of a float, without bit_cast (the CI's GCC 9 library lacks it).
std::uint32_t float_bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

float bits_float(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

class Writer {
public:
    void u8(std::uint8_t value) {
        out_.push_back(static_cast<char>(value));
    }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out_.push_back(static_cast<char>((value >> shift) & 0xFFu));
        }
    }
    void i32(std::int32_t value) {
        u32(static_cast<std::uint32_t>(value));
    }
    void f32(float value) {
        u32(float_bits(value));
    }
    void text(const std::string& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        out_.append(value);
    }
    void floats(const float* values, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            f32(values[i]);
        }
    }
    [[nodiscard]] std::string take() {
        return std::move(out_);
    }

private:
    std::string out_;
};

class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    [[nodiscard]] bool at_end() const noexcept {
        return pos_ == bytes_.size();
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - pos_;
    }

    void need(std::size_t count) const {
        if (count > remaining()) {
            throw ReadError{"the payload ends early"};
        }
    }

    // Checks that `count` elements of `element_bytes` each fit in the rest of the file, before the caller allocates for them.
    void need_elements(std::uint64_t count, std::size_t element_bytes) const {
        if (count > remaining() / element_bytes) {
            throw ReadError{"an element count is larger than the payload"};
        }
    }

    std::uint8_t u8() {
        need(1);
        return static_cast<std::uint8_t>(bytes_[pos_++]);
    }
    std::uint32_t u32() {
        need(4);
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes_[pos_++])) << shift;
        }
        return value;
    }
    std::int32_t i32() {
        return static_cast<std::int32_t>(u32());
    }
    float f32() {
        return bits_float(u32());
    }
    std::string text() {
        const std::uint32_t length = u32();
        need(length);
        std::string value(bytes_.substr(pos_, length));
        pos_ += length;
        if (!core::is_valid_utf8(value)) {
            throw ReadError{"a string is not valid UTF-8"};
        }
        return value;
    }
    void floats(float* values, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            values[i] = f32();
        }
    }

private:
    std::string_view bytes_;
    std::size_t pos_ = 0;
};

bool finite(float value) {
    return std::isfinite(value);
}

bool finite(const glm::vec2& value) {
    return finite(value.x) && finite(value.y);
}

bool finite(const glm::vec3& value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finite(const glm::vec4& value) {
    return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
}

bool finite(const glm::mat4& value) {
    for (int column = 0; column < 4; ++column) {
        if (!finite(value[column])) {
            return false;
        }
    }
    return true;
}

template <typename T>
bool all_finite(const std::vector<T>& values) {
    for (const T& value : values) {
        if (!finite(value)) {
            return false;
        }
    }
    return true;
}

// Mesh and material labels in messages use their position, because names may be empty or repeated.
std::string mesh_label(std::size_t index) {
    return "mesh " + std::to_string(index);
}

std::string material_label(std::size_t index) {
    return "material " + std::to_string(index);
}

std::string node_label(std::size_t index) {
    return "node " + std::to_string(index);
}

// The box must be exactly the box of the positions. The sphere is centered on the box and must reach every vertex; the
// margin is a small fraction of the mesh size, since the center is a float average.
Result<Unit, std::string> validate_bounds(const MeshBounds& bounds, const std::vector<glm::vec3>& positions) {
    if (!finite(bounds.min) || !finite(bounds.max) || !finite(bounds.center) || !std::isfinite(bounds.radius) ||
        bounds.radius < 0.0f) {
        return failure(std::string("the bounds are not finite"));
    }
    if (positions.empty()) {
        if (bounds.min != glm::vec3(0.0f) || bounds.max != glm::vec3(0.0f) || bounds.center != glm::vec3(0.0f) ||
            bounds.radius != 0.0f) {
            return failure(std::string("the bounds are set for a mesh with no vertices"));
        }
        return success();
    }
    glm::vec3 lo = positions.front();
    glm::vec3 hi = positions.front();
    for (const glm::vec3& position : positions) {
        lo = glm::min(lo, position);
        hi = glm::max(hi, position);
    }
    if (bounds.min != lo || bounds.max != hi) {
        return failure(std::string("the bounding box is not the box of the vertices"));
    }
    const float tolerance = kBoundsTolerance * std::max(1.0f, glm::length(hi - lo));
    if (glm::length(bounds.center - (lo + hi) * 0.5f) > tolerance) {
        return failure(std::string("the bounding sphere is not centered on the box"));
    }
    for (const glm::vec3& position : positions) {
        if (glm::length(position - bounds.center) > bounds.radius + tolerance) {
            return failure(std::string("the bounding sphere does not reach every vertex"));
        }
    }
    return success();
}

Result<Unit, std::string> validate_meshlets(const ModelMesh& mesh, std::size_t lod_index);

// The rules for one LOD. Triangles index the mesh's vertices, the error and the triangle count do not shrink or grow in
// the wrong direction, and the meshlets reproduce the LOD's triangles in order within their limits and bounds.
Result<Unit, std::string> validate_lod(const ModelMesh& mesh, std::size_t lod_index) {
    const MeshLod& lod = mesh.lods[lod_index];
    const std::string label = "LOD " + std::to_string(lod_index);
    if (lod.indices.size() % 3 != 0) {
        return failure(label + ": the index count is not a multiple of 3");
    }
    const std::size_t vertex_count = mesh.positions.size();
    for (std::uint32_t index_value : lod.indices) {
        if (index_value >= vertex_count) {
            return failure(label + ": an index is out of range");
        }
    }
    if (!std::isfinite(lod.error) || lod.error < 0.0f) {
        return failure(label + ": the error is not a finite non-negative number");
    }
    if (lod_index == 0) {
        if (lod.error != 0.0f) {
            return failure(label + ": LOD 0 must have zero error");
        }
    } else {
        const MeshLod& finer = mesh.lods[lod_index - 1];
        if (lod.error < finer.error) {
            return failure(label + ": the error is smaller than the error of the finer LOD");
        }
        if (lod.indices.size() > finer.indices.size()) {
            return failure(label + ": has more triangles than the finer LOD");
        }
    }
    return validate_meshlets(mesh, lod_index);
}

Result<Unit, std::string> validate_meshlets(const ModelMesh& mesh, std::size_t lod_index) {
    const MeshLod& lod = mesh.lods[lod_index];
    const std::string label = "LOD " + std::to_string(lod_index);
    const std::size_t vertex_count = mesh.positions.size();
    if (lod.meshlet_triangles.size() != lod.indices.size()) {
        return failure(label + ": the meshlet triangles do not cover the LOD's triangles");
    }
    // Meshlets are packed back to back. The triangles they hold must be the LOD's triangles: the same set, though the
    // meshlet builder may reorder them and rotate their corners.
    std::size_t vertex_cursor = 0;
    std::size_t triangle_cursor = 0;  // in bytes
    std::vector<Triangle> meshlet_triangles;
    meshlet_triangles.reserve(lod.indices.size() / 3);
    for (std::size_t m = 0; m < lod.meshlets.size(); ++m) {
        const Meshlet& meshlet = lod.meshlets[m];
        const std::string meshlet_label = label + ", meshlet " + std::to_string(m);
        if (meshlet.vertex_count == 0 || meshlet.vertex_count > kMeshletMaxVertices) {
            return failure(meshlet_label + ": the vertex count is not between 1 and " + std::to_string(kMeshletMaxVertices));
        }
        if (meshlet.triangle_count == 0 || meshlet.triangle_count > kMeshletMaxTriangles) {
            return failure(meshlet_label + ": the triangle count is not between 1 and " + std::to_string(kMeshletMaxTriangles));
        }
        if (meshlet.vertex_offset != vertex_cursor || meshlet.triangle_offset != triangle_cursor) {
            return failure(meshlet_label + ": the meshlets are not packed in order");
        }
        const std::size_t triangle_bytes = 3 * static_cast<std::size_t>(meshlet.triangle_count);
        if (vertex_cursor + meshlet.vertex_count > lod.meshlet_vertices.size() ||
            triangle_cursor + triangle_bytes > lod.meshlet_triangles.size()) {
            return failure(meshlet_label + ": the meshlet data is out of range");
        }
        const std::uint32_t* vertices = lod.meshlet_vertices.data() + vertex_cursor;
        const std::uint8_t* triangles = lod.meshlet_triangles.data() + triangle_cursor;
        vertex_cursor += meshlet.vertex_count;
        triangle_cursor += triangle_bytes;

        for (std::uint32_t k = 0; k < meshlet.vertex_count; ++k) {
            if (vertices[k] >= vertex_count) {
                return failure(meshlet_label + ": a vertex is out of range");
            }
        }
        for (std::size_t t = 0; t < meshlet.triangle_count; ++t) {
            Triangle triangle{};
            for (std::size_t k = 0; k < 3; ++k) {
                const std::uint8_t local = triangles[3 * t + k];
                if (local >= meshlet.vertex_count) {
                    return failure(meshlet_label + ": a triangle refers past the meshlet's vertices");
                }
                triangle[k] = vertices[local];
            }
            meshlet_triangles.push_back(canonical_triangle(triangle));
        }

        if (!finite(meshlet.center) || !std::isfinite(meshlet.radius) || meshlet.radius < 0.0f ||
            !finite(meshlet.cone_axis) || !std::isfinite(meshlet.cone_cutoff) || meshlet.cone_cutoff < -1.0f ||
            meshlet.cone_cutoff > 1.0f || glm::length(meshlet.cone_axis) > 1.001f) {
            return failure(meshlet_label + ": a bound is not finite or is out of range");
        }
        const float tolerance = kBoundsTolerance * std::max(1.0f, meshlet.radius);
        for (std::uint32_t k = 0; k < meshlet.vertex_count; ++k) {
            if (glm::length(mesh.positions[vertices[k]] - meshlet.center) > meshlet.radius + tolerance) {
                return failure(meshlet_label + ": its sphere does not reach its vertices");
            }
        }
    }
    if (vertex_cursor != lod.meshlet_vertices.size() || triangle_cursor != lod.meshlet_triangles.size()) {
        return failure(label + ": the meshlets do not cover the LOD exactly");
    }
    std::vector<Triangle> expected;
    expected.reserve(lod.indices.size() / 3);
    for (std::size_t t = 0; t < lod.indices.size(); t += 3) {
        expected.push_back(canonical_triangle({lod.indices[t], lod.indices[t + 1], lod.indices[t + 2]}));
    }
    std::sort(meshlet_triangles.begin(), meshlet_triangles.end());
    std::sort(expected.begin(), expected.end());
    if (meshlet_triangles != expected) {
        return failure(label + ": the meshlets do not reproduce the LOD's triangles");
    }
    return success();
}

Result<Unit, std::string> validate_mesh(const ModelPayload& model, std::size_t index) {
    const ModelMesh& mesh = model.meshes[index];
    const std::string label = mesh_label(index);
    if (!core::is_valid_utf8(mesh.name)) {
        return failure(label + ": the name is not valid UTF-8");
    }
    if (mesh.material < -1 || mesh.material >= static_cast<std::int32_t>(model.materials.size())) {
        return failure(label + ": the material index is out of range");
    }
    const std::size_t vertex_count = mesh.positions.size();
    if ((!mesh.normals.empty() && mesh.normals.size() != vertex_count) ||
        (!mesh.tangents.empty() && mesh.tangents.size() != vertex_count) ||
        (!mesh.uvs.empty() && mesh.uvs.size() != vertex_count)) {
        return failure(label + ": an attribute array does not have one entry per vertex");
    }
    if (!all_finite(mesh.positions) || !all_finite(mesh.normals) || !all_finite(mesh.tangents) ||
        !all_finite(mesh.uvs)) {
        return failure(label + ": a vertex attribute is not finite");
    }
    if (auto bounds = validate_bounds(mesh.bounds, mesh.positions); !bounds.ok()) {
        return failure(label + ": " + bounds.error());
    }
    if (mesh.lods.empty() || mesh.lods.size() > kMaxLodCount) {
        return failure(label + ": the LOD count is not between 1 and " + std::to_string(kMaxLodCount));
    }
    for (std::size_t lod = 0; lod < mesh.lods.size(); ++lod) {
        if (auto checked = validate_lod(mesh, lod); !checked.ok()) {
            return failure(label + ": " + checked.error());
        }
    }
    // LOD 0 is the full detail, so every vertex is in it. A vertex it does not use would be dead weight, and the cook
    // step removes those.
    std::vector<bool> used(vertex_count, false);
    for (std::uint32_t index_value : mesh.lods[0].indices) {
        used[index_value] = true;
    }
    if (std::find(used.begin(), used.end(), false) != used.end()) {
        return failure(label + ": a vertex is not used by LOD 0");
    }
    return success();
}

Result<Unit, std::string> validate_material(const ModelMaterial& material, std::size_t index) {
    const std::string label = material_label(index);
    const std::string* texts[] = {&material.name, &material.base_color_texture, &material.normal_texture,
                                  &material.metallic_roughness_texture, &material.emissive_texture};
    for (const std::string* text : texts) {
        if (!core::is_valid_utf8(*text)) {
            return failure(label + ": a string is not valid UTF-8");
        }
    }
    if (static_cast<std::uint32_t>(material.alpha_mode) > kLastAlphaMode) {
        return failure(label + ": the alpha mode is unknown");
    }
    const bool numbers_finite = finite(material.base_color) && finite(material.metallic) && finite(material.roughness) &&
                                finite(material.emissive) && finite(material.alpha_cutoff);
    if (!numbers_finite) {
        return failure(label + ": a number is not finite");
    }
    return success();
}

Result<Unit, std::string> validate_node(const ModelPayload& model, std::size_t index) {
    const ModelNode& node = model.nodes[index];
    const std::string label = node_label(index);
    if (!core::is_valid_utf8(node.name)) {
        return failure(label + ": the name is not valid UTF-8");
    }
    // A parent must come earlier in the list. That keeps the hierarchy acyclic and lets readers compose transforms in
    // one forward pass.
    if (node.parent < -1 || node.parent >= static_cast<std::int32_t>(index)) {
        return failure(label + ": the parent must be -1 or an earlier node");
    }
    for (std::uint32_t mesh : node.meshes) {
        if (mesh >= model.meshes.size()) {
            return failure(label + ": a mesh index is out of range");
        }
    }
    if (!finite(node.local)) {
        return failure(label + ": the local transform is not finite");
    }
    return success();
}

void read_lod(Reader& in, MeshLod& lod);

ModelMesh read_mesh(Reader& in, std::uint32_t version) {
    ModelMesh mesh;
    mesh.name = in.text();
    mesh.material = in.i32();
    const std::uint32_t vertex_count = in.u32();
    const std::uint32_t flags = in.u32();
    if ((flags & ~kKnownFlags) != 0 || ((flags & kFlagSkin) != 0 && version < 3)) {
        throw ReadError{"a mesh has unknown attribute flags"};
    }
    in.need_elements(vertex_count, 3 * sizeof(float));
    mesh.positions.resize(vertex_count);
    for (glm::vec3& position : mesh.positions) {
        in.floats(&position.x, 3);
    }
    if ((flags & kFlagNormals) != 0) {
        in.need_elements(vertex_count, 3 * sizeof(float));
        mesh.normals.resize(vertex_count);
        for (glm::vec3& normal : mesh.normals) {
            in.floats(&normal.x, 3);
        }
    }
    if ((flags & kFlagTangents) != 0) {
        in.need_elements(vertex_count, 4 * sizeof(float));
        mesh.tangents.resize(vertex_count);
        for (glm::vec4& tangent : mesh.tangents) {
            in.floats(&tangent.x, 4);
        }
    }
    if ((flags & kFlagUvs) != 0) {
        in.need_elements(vertex_count, 2 * sizeof(float));
        mesh.uvs.resize(vertex_count);
        for (glm::vec2& uv : mesh.uvs) {
            in.floats(&uv.x, 2);
        }
    }
    if ((flags & kFlagSkin) != 0) {
        in.need_elements(vertex_count, 8 * sizeof(float));
        mesh.joints.resize(vertex_count);
        mesh.weights.resize(vertex_count);
        for (std::uint32_t v = 0; v < vertex_count; ++v) {
            for (int k = 0; k < 4; ++k) {
                mesh.joints[v][k] = in.u32();
            }
            in.floats(&mesh.weights[v].x, 4);
        }
    }
    in.floats(&mesh.bounds.min.x, 3);
    in.floats(&mesh.bounds.max.x, 3);
    in.floats(&mesh.bounds.center.x, 3);
    mesh.bounds.radius = in.f32();

    const std::uint32_t lod_count = in.u32();
    if (lod_count == 0 || lod_count > kMaxLodCount) {
        throw ReadError{"a mesh has a LOD count that is not between 1 and 4"};
    }
    in.need_elements(lod_count, kLodMinBytes);
    mesh.lods.resize(lod_count);
    for (MeshLod& lod : mesh.lods) {
        read_lod(in, lod);
    }
    return mesh;
}

void read_lod(Reader& in, MeshLod& lod) {
    lod.error = in.f32();
    const std::uint32_t index_count = in.u32();
    in.need_elements(index_count, sizeof(std::uint32_t));
    lod.indices.resize(index_count);
    for (std::uint32_t& index : lod.indices) {
        index = in.u32();
    }

    const std::uint32_t meshlet_count = in.u32();
    in.need_elements(meshlet_count, kMeshletBytes);
    lod.meshlets.resize(meshlet_count);
    for (Meshlet& meshlet : lod.meshlets) {
        meshlet.vertex_offset = in.u32();
        meshlet.vertex_count = in.u32();
        meshlet.triangle_offset = in.u32();
        meshlet.triangle_count = in.u32();
        in.floats(&meshlet.center.x, 3);
        meshlet.radius = in.f32();
        in.floats(&meshlet.cone_axis.x, 3);
        meshlet.cone_cutoff = in.f32();
    }

    const std::uint32_t meshlet_vertex_count = in.u32();
    in.need_elements(meshlet_vertex_count, sizeof(std::uint32_t));
    lod.meshlet_vertices.resize(meshlet_vertex_count);
    for (std::uint32_t& vertex : lod.meshlet_vertices) {
        vertex = in.u32();
    }

    const std::uint32_t meshlet_triangle_bytes = in.u32();
    in.need_elements(meshlet_triangle_bytes, sizeof(std::uint8_t));
    lod.meshlet_triangles.resize(meshlet_triangle_bytes);
    for (std::uint8_t& byte : lod.meshlet_triangles) {
        byte = in.u8();
    }
}

ModelMaterial read_material(Reader& in) {
    ModelMaterial material;
    material.name = in.text();
    in.floats(&material.base_color.x, 4);
    material.metallic = in.f32();
    material.roughness = in.f32();
    in.floats(&material.emissive.x, 3);
    material.alpha_mode = static_cast<AlphaMode>(in.u32());
    material.alpha_cutoff = in.f32();
    const std::uint8_t double_sided = in.u8();
    if (double_sided > 1) {
        throw ReadError{"a material has a double-sided flag that is not 0 or 1"};
    }
    material.double_sided = double_sided == 1;
    material.base_color_texture = in.text();
    material.normal_texture = in.text();
    material.metallic_roughness_texture = in.text();
    material.emissive_texture = in.text();
    return material;
}

ModelNode read_node(Reader& in) {
    ModelNode node;
    node.name = in.text();
    node.parent = in.i32();
    in.floats(&node.local[0].x, 16);
    const std::uint32_t mesh_count = in.u32();
    in.need_elements(mesh_count, sizeof(std::uint32_t));
    node.meshes.resize(mesh_count);
    for (std::uint32_t& mesh : node.meshes) {
        mesh = in.u32();
    }
    return node;
}

void read_keys3(Reader& in, std::vector<ModelKey3>& keys) {
    const std::uint32_t count = in.u32();
    in.need_elements(count, 4 * sizeof(float));
    keys.resize(count);
    for (ModelKey3& key : keys) {
        key.time = in.f32();
        in.floats(&key.value.x, 3);
    }
}

void read_animation(Reader& in, ModelPayload& model) {
    const std::uint32_t joint_count = in.u32();
    in.need_elements(joint_count, 4 + 4 + 4 + 16 * sizeof(float));
    model.joints.resize(joint_count);
    for (ModelJoint& joint : model.joints) {
        joint.name = in.text();
        joint.parent = in.i32();
        joint.node = in.i32();
        in.floats(&joint.inverse_bind[0].x, 16);
    }
    const std::uint32_t clip_count = in.u32();
    in.need_elements(clip_count, 4 + 4 + 4);
    model.clips.resize(clip_count);
    for (ModelClip& clip : model.clips) {
        clip.name = in.text();
        clip.duration = in.f32();
        const std::uint32_t channel_count = in.u32();
        in.need_elements(channel_count, 4 + 3 * 4);
        clip.channels.resize(channel_count);
        for (ModelChannel& channel : clip.channels) {
            channel.joint = in.u32();
            read_keys3(in, channel.translation);
            const std::uint32_t rotation_count = in.u32();
            in.need_elements(rotation_count, 5 * sizeof(float));
            channel.rotation.resize(rotation_count);
            for (ModelKeyRotation& key : channel.rotation) {
                key.time = in.f32();
                in.floats(&key.value.x, 4);
            }
            read_keys3(in, channel.scale);
        }
    }
}

ModelPayload parse_payload(std::string_view bytes) {
    Reader in(bytes);
    if (bytes.substr(0, kModelPayloadMagic.size()) != kModelPayloadMagic) {
        throw ReadError{"not a model payload (bad magic)"};
    }
    for (std::size_t i = 0; i < kModelPayloadMagic.size(); ++i) {
        in.u8();
    }
    const std::uint32_t version = in.u32();
    if (version != kStaticVersion && version != kModelPayloadVersion) {
        throw ReadError{"unsupported model payload version"};
    }

    ModelPayload model;
    const std::uint32_t mesh_count = in.u32();
    in.need_elements(mesh_count, kMeshMinBytes);
    model.meshes.reserve(mesh_count);
    for (std::uint32_t i = 0; i < mesh_count; ++i) {
        model.meshes.push_back(read_mesh(in, version));
    }

    const std::uint32_t material_count = in.u32();
    in.need_elements(material_count, kMaterialMinBytes);
    model.materials.reserve(material_count);
    for (std::uint32_t i = 0; i < material_count; ++i) {
        model.materials.push_back(read_material(in));
    }

    const std::uint32_t node_count = in.u32();
    in.need_elements(node_count, kNodeMinBytes);
    model.nodes.reserve(node_count);
    for (std::uint32_t i = 0; i < node_count; ++i) {
        model.nodes.push_back(read_node(in));
    }

    if (version >= 3) {
        read_animation(in, model);
    }
    if (!in.at_end()) {
        throw ReadError{"the payload has bytes after its last section"};
    }
    return model;
}

}  // namespace

std::string write_model_payload(const ModelPayload& model) {
    Writer out;
    for (char letter : kModelPayloadMagic) {
        out.u8(static_cast<std::uint8_t>(letter));
    }
    const bool animated = !model.joints.empty() || !model.clips.empty();
    out.u32(animated ? kModelPayloadVersion : kStaticVersion);

    out.u32(static_cast<std::uint32_t>(model.meshes.size()));
    for (const ModelMesh& mesh : model.meshes) {
        out.text(mesh.name);
        out.i32(mesh.material);
        out.u32(static_cast<std::uint32_t>(mesh.positions.size()));
        std::uint32_t flags = 0;
        flags |= mesh.normals.empty() ? 0 : kFlagNormals;
        flags |= mesh.tangents.empty() ? 0 : kFlagTangents;
        flags |= mesh.uvs.empty() ? 0 : kFlagUvs;
        flags |= mesh.joints.empty() ? 0 : kFlagSkin;
        out.u32(flags);
        for (const glm::vec3& position : mesh.positions) {
            out.floats(&position.x, 3);
        }
        for (const glm::vec3& normal : mesh.normals) {
            out.floats(&normal.x, 3);
        }
        for (const glm::vec4& tangent : mesh.tangents) {
            out.floats(&tangent.x, 4);
        }
        for (const glm::vec2& uv : mesh.uvs) {
            out.floats(&uv.x, 2);
        }
        for (std::size_t v = 0; v < mesh.joints.size(); ++v) {
            for (int k = 0; k < 4; ++k) {
                out.u32(mesh.joints[v][k]);
            }
            out.floats(&mesh.weights[v].x, 4);
        }
        out.floats(&mesh.bounds.min.x, 3);
        out.floats(&mesh.bounds.max.x, 3);
        out.floats(&mesh.bounds.center.x, 3);
        out.f32(mesh.bounds.radius);

        out.u32(static_cast<std::uint32_t>(mesh.lods.size()));
        for (const MeshLod& lod : mesh.lods) {
            out.f32(lod.error);
            out.u32(static_cast<std::uint32_t>(lod.indices.size()));
            for (std::uint32_t index : lod.indices) {
                out.u32(index);
            }
            out.u32(static_cast<std::uint32_t>(lod.meshlets.size()));
            for (const Meshlet& meshlet : lod.meshlets) {
                out.u32(meshlet.vertex_offset);
                out.u32(meshlet.vertex_count);
                out.u32(meshlet.triangle_offset);
                out.u32(meshlet.triangle_count);
                out.floats(&meshlet.center.x, 3);
                out.f32(meshlet.radius);
                out.floats(&meshlet.cone_axis.x, 3);
                out.f32(meshlet.cone_cutoff);
            }
            out.u32(static_cast<std::uint32_t>(lod.meshlet_vertices.size()));
            for (std::uint32_t vertex : lod.meshlet_vertices) {
                out.u32(vertex);
            }
            out.u32(static_cast<std::uint32_t>(lod.meshlet_triangles.size()));
            for (std::uint8_t byte : lod.meshlet_triangles) {
                out.u8(byte);
            }
        }
    }

    out.u32(static_cast<std::uint32_t>(model.materials.size()));
    for (const ModelMaterial& material : model.materials) {
        out.text(material.name);
        out.floats(&material.base_color.x, 4);
        out.f32(material.metallic);
        out.f32(material.roughness);
        out.floats(&material.emissive.x, 3);
        out.u32(static_cast<std::uint32_t>(material.alpha_mode));
        out.f32(material.alpha_cutoff);
        out.u8(material.double_sided ? 1 : 0);
        out.text(material.base_color_texture);
        out.text(material.normal_texture);
        out.text(material.metallic_roughness_texture);
        out.text(material.emissive_texture);
    }

    out.u32(static_cast<std::uint32_t>(model.nodes.size()));
    for (const ModelNode& node : model.nodes) {
        out.text(node.name);
        out.i32(node.parent);
        // Column-major: the 16 floats are the matrix's columns in order, as glm stores them.
        out.floats(&node.local[0].x, 16);
        out.u32(static_cast<std::uint32_t>(node.meshes.size()));
        for (std::uint32_t mesh : node.meshes) {
            out.u32(mesh);
        }
    }
    if (animated) {
        out.u32(static_cast<std::uint32_t>(model.joints.size()));
        for (const ModelJoint& joint : model.joints) {
            out.text(joint.name);
            out.i32(joint.parent);
            out.i32(joint.node);
            out.floats(&joint.inverse_bind[0].x, 16);
        }
        out.u32(static_cast<std::uint32_t>(model.clips.size()));
        for (const ModelClip& clip : model.clips) {
            out.text(clip.name);
            out.f32(clip.duration);
            out.u32(static_cast<std::uint32_t>(clip.channels.size()));
            for (const ModelChannel& channel : clip.channels) {
                out.u32(channel.joint);
                out.u32(static_cast<std::uint32_t>(channel.translation.size()));
                for (const ModelKey3& key : channel.translation) {
                    out.f32(key.time);
                    out.floats(&key.value.x, 3);
                }
                out.u32(static_cast<std::uint32_t>(channel.rotation.size()));
                for (const ModelKeyRotation& key : channel.rotation) {
                    out.f32(key.time);
                    out.floats(&key.value.x, 4);
                }
                out.u32(static_cast<std::uint32_t>(channel.scale.size()));
                for (const ModelKey3& key : channel.scale) {
                    out.f32(key.time);
                    out.floats(&key.value.x, 3);
                }
            }
        }
    }
    return out.take();
}

Result<Unit, std::string> validate_animation(const ModelPayload& model) {
    for (std::size_t i = 0; i < model.joints.size(); ++i) {
        const ModelJoint& joint = model.joints[i];
        const std::string label = "joint " + std::to_string(i);
        if (joint.parent < -1 || joint.parent >= static_cast<std::int32_t>(i)) {
            return failure(label + " has a parent that is not an earlier joint");
        }
        if (joint.node < -1 || joint.node >= static_cast<std::int32_t>(model.nodes.size())) {
            return failure(label + " names a node that does not exist");
        }
        if (!finite(joint.inverse_bind)) {
            return failure(label + " has a non-finite inverse bind matrix");
        }
    }
    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const ModelMesh& mesh = model.meshes[m];
        if (mesh.joints.size() != mesh.weights.size() || (!mesh.joints.empty() && mesh.joints.size() != mesh.positions.size())) {
            return failure(mesh_label(m) + " has skin arrays that do not match its vertices");
        }
        for (std::size_t v = 0; v < mesh.joints.size(); ++v) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                if (mesh.joints[v][k] >= model.joints.size()) {
                    return failure(mesh_label(m) + " has a vertex influenced by a joint that does not exist");
                }
                const float weight = mesh.weights[v][k];
                if (!(weight >= 0.0f) || !std::isfinite(weight)) {
                    return failure(mesh_label(m) + " has a negative or non-finite skin weight");
                }
                sum += weight;
            }
            if (std::abs(sum - 1.0f) > 1.0e-3f) {
                return failure(mesh_label(m) + " has skin weights that do not sum to one");
            }
        }
    }
    for (std::size_t c = 0; c < model.clips.size(); ++c) {
        const ModelClip& clip = model.clips[c];
        const std::string label = "clip " + std::to_string(c);
        if (!(clip.duration > 0.0f) || !std::isfinite(clip.duration)) {
            return failure(label + " has a duration that is not positive");
        }
        const auto times_ok = [&clip](const auto& keys) {
            float previous = 0.0f;
            for (const auto& key : keys) {
                if (!std::isfinite(key.time) || key.time < previous || key.time > clip.duration + 1.0e-3f) {
                    return false;
                }
                previous = key.time;
            }
            return true;
        };
        for (const ModelChannel& channel : clip.channels) {
            if (channel.joint >= model.joints.size()) {
                return failure(label + " animates a joint that does not exist");
            }
            if (!times_ok(channel.translation) || !times_ok(channel.rotation) || !times_ok(channel.scale)) {
                return failure(label + " has key times that are not in order inside the clip");
            }
            for (const ModelKeyRotation& key : channel.rotation) {
                if (!finite(key.value)) {
                    return failure(label + " has a non-finite rotation key");
                }
            }
            for (const ModelKey3& key : channel.translation) {
                if (!finite(key.value)) {
                    return failure(label + " has a non-finite translation key");
                }
            }
            for (const ModelKey3& key : channel.scale) {
                if (!finite(key.value)) {
                    return failure(label + " has a non-finite scale key");
                }
            }
        }
    }
    return success();
}

Result<Unit, std::string> validate_model(const ModelPayload& model) {
    for (std::size_t i = 0; i < model.materials.size(); ++i) {
        if (auto checked = validate_material(model.materials[i], i); !checked.ok()) {
            return checked;
        }
    }
    for (std::size_t i = 0; i < model.meshes.size(); ++i) {
        if (auto checked = validate_mesh(model, i); !checked.ok()) {
            return checked;
        }
    }
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        if (auto checked = validate_node(model, i); !checked.ok()) {
            return checked;
        }
    }
    return validate_animation(model);
}

Result<ModelPayload, std::string> read_model_payload(std::string_view bytes) {
    try {
        ModelPayload model = parse_payload(bytes);
        if (auto checked = validate_model(model); !checked.ok()) {
            return failure(std::move(checked).error());
        }
        return success(std::move(model));
    } catch (const ReadError& error) {
        return failure(error.message);
    }
}

}  // namespace engine::converter
