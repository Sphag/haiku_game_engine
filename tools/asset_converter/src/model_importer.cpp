// The "model" importer (US-020): reads glTF/GLB, FBX, and OBJ through Assimp and writes the engine's model payload.
//
// Conventions. Assimp returns +Y up and right-handed data for all three formats, which is the engine's frame, so the axes
// need no conversion here. Units differ: glTF is in meters, OBJ has no declared unit and is taken as meters, and FBX is
// scaled by its UnitScaleFactor (centimeters per unit) divided by 100. The "scale" setting is applied to the root node.
// Geometry stays in each node's local space, so only the root transform changes.
//
// Texture coordinates use the glTF convention, with v = 0 at the top row of the image. Assimp hands glTF over with v
// already flipped and OBJ and FBX with v as the file wrote it, so every format gets v = 1 - v here. Tangents are not taken
// from Assimp's tangent-space pass, which runs in its own UV space: glTF tangents that the file provides are kept, and all
// other tangents are computed from the engine's UVs.
//
// Limits. External files that a source names (a glTF .bin, an OBJ .mtl) are not read, because the importer receives only
// the source bytes. Each refused file is reported, and its data falls back to defaults. Embedded textures and textures in
// general belong to US-021, and skinning and animation belong to US-022; all of these are reported, not dropped silently.

#include "asset_converter/importer.hpp"
#include "asset_converter/mesh_cooking.hpp"
#include "asset_converter/model_format.hpp"

#include "engine/core/utf8.hpp"

#include <assimp/GltfMaterial.h>
#include <assimp/anim.h>
#include <assimp/Importer.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::converter {
namespace {

using Json = nlohmann::json;
using core::Result;
using core::failure;
using core::success;

enum class Format { Gltf, Fbx, Obj };

// FBX's UnitScaleFactor is centimeters per unit when it is 1; a file that counts meters sets it to 100.
constexpr double kFbxCentimetersPerMeter = 100.0;
constexpr double kFbxUnitScaleDefault = 1.0;

// The postprocessing that Assimp runs on every source. It triangulates, merges identical vertices, generates normals where
// the source has none, and validates the structure, so the importer can trust the arrays it reads.
constexpr unsigned int kPostprocessFlags =
    aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenSmoothNormals | aiProcess_ValidateDataStructure;

// Recognizes the source from its first bytes. The OBJ check is the fallback, because OBJ has no signature; a file that is
// not OBJ either fails inside Assimp with its own message.
Format detect_format(std::string_view source) {
    if (source.substr(0, 4) == "glTF") {
        return Format::Gltf;
    }
    if (source.substr(0, 18) == "Kaydara FBX Binary") {
        return Format::Fbx;
    }
    std::size_t start = 0;
    if (source.substr(0, 3) == "\xEF\xBB\xBF") {
        start = 3;
    }
    while (start < source.size() && (source[start] == ' ' || source[start] == '\t' || source[start] == '\r' ||
                                     source[start] == '\n')) {
        ++start;
    }
    const std::string_view body = source.substr(start);
    if (body.substr(0, 1) == "{") {
        return Format::Gltf;
    }
    if (body.substr(0, 5) == "; FBX") {
        return Format::Fbx;
    }
    return Format::Obj;
}

const char* hint_for(Format format) {
    switch (format) {
        case Format::Gltf:
            return "gltf";
        case Format::Fbx:
            return "fbx";
        case Format::Obj:
            return "obj";
    }
    return "";
}

// Assimp reads a source's external files through this stream system. Only the source bytes were given, so no external
// file can be resolved. Each request is recorded as a note, and Assimp is told that the file does not exist.
// Assimp may ask for the same file more than once, so each file is noted once.
class RefuseExternalFiles final : public Assimp::IOSystem {
public:
    explicit RefuseExternalFiles(std::vector<std::string>& notes) : notes_(notes) {}

    [[nodiscard]] bool Exists(const char*) const override {
        return false;
    }
    [[nodiscard]] char getOsSeparator() const override {
        return '/';
    }
    Assimp::IOStream* Open(const char* path, const char*) override {
        const std::string name = path;
        if (std::find(requested_.begin(), requested_.end(), name) == requested_.end()) {
            requested_.push_back(name);
            notes_.push_back("external file '" + name + "' is not read; the import uses defaults for the data it would supply");
        }
        return nullptr;
    }
    void Close(Assimp::IOStream*) override {}

private:
    std::vector<std::string>& notes_;
    std::vector<std::string> requested_;
};

// Assimp matrices are row-major with translation in the last column; glm takes its arguments column by column.
glm::mat4 to_glm(const aiMatrix4x4& m) {
    return glm::mat4(m.a1, m.b1, m.c1, m.d1, m.a2, m.b2, m.c2, m.d2, m.a3, m.b3, m.c3, m.d3, m.a4, m.b4, m.c4, m.d4);
}

std::string label_for(const aiMesh& mesh, unsigned int index) {
    const std::string name = mesh.mName.C_Str();
    return "mesh " + std::to_string(index) + (name.empty() ? "" : " '" + name + "'");
}

// The texture path that Assimp reports for a slot, or empty when the slot is unused. An embedded texture has no file, so
// it is reported and left empty.
std::string texture_path(const aiMaterial& material, aiTextureType type, unsigned int slot, std::vector<std::string>& notes,
                         const std::string& material_name) {
    if (material.GetTextureCount(type) == 0) {
        return {};
    }
    aiString path;
    if (material.GetTexture(type, slot, &path) != AI_SUCCESS) {
        return {};
    }
    if (path.length > 0 && path.C_Str()[0] == '*') {
        notes.push_back("material '" + material_name +
                        "' uses an embedded texture; embedded textures are not imported yet (US-021)");
        return {};
    }
    return path.C_Str();
}

ModelMaterial convert_material(const aiMaterial& material, Format format, std::vector<std::string>& notes) {
    ModelMaterial out;
    aiString name;
    if (material.Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
        out.name = name.C_Str();
    }

    // glTF stores the base color with alpha; OBJ and FBX store a diffuse color and an opacity.
    aiColor4D base{1.0f, 1.0f, 1.0f, 1.0f};
    if (material.Get(AI_MATKEY_BASE_COLOR, base) != AI_SUCCESS) {
        aiColor3D diffuse{1.0f, 1.0f, 1.0f};
        if (material.Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS) {
            base = aiColor4D(diffuse.r, diffuse.g, diffuse.b, 1.0f);
        }
        float opacity = 1.0f;
        if (material.Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
            base.a = opacity;
        }
    }
    out.base_color = glm::vec4(base.r, base.g, base.b, base.a);

    // glTF's defaults for an absent metallic-roughness block are metallic 1 and roughness 1; other formats have none, so
    // they get a non-metal with roughness 1. A failed Get leaves its output unchanged, so the default stays.
    float metallic = format == Format::Gltf ? 1.0f : 0.0f;
    material.Get(AI_MATKEY_METALLIC_FACTOR, metallic);
    out.metallic = metallic;
    float roughness = 1.0f;
    material.Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);
    out.roughness = roughness;

    aiColor3D emissive{0.0f, 0.0f, 0.0f};
    material.Get(AI_MATKEY_COLOR_EMISSIVE, emissive);
    out.emissive = glm::vec3(emissive.r, emissive.g, emissive.b);

    int two_sided = 0;
    material.Get(AI_MATKEY_TWOSIDED, two_sided);
    out.double_sided = two_sided != 0;

    aiString alpha_mode;
    if (material.Get(AI_MATKEY_GLTF_ALPHAMODE, alpha_mode) == AI_SUCCESS) {
        const std::string mode = alpha_mode.C_Str();
        if (mode == "MASK") {
            out.alpha_mode = AlphaMode::Mask;
        } else if (mode == "BLEND") {
            out.alpha_mode = AlphaMode::Blend;
        }
    } else if (base.a < 1.0f) {
        out.alpha_mode = AlphaMode::Blend;
    }
    float cutoff = 0.5f;
    material.Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff);
    out.alpha_cutoff = cutoff;

    // The base color texture is the glTF slot, or the diffuse slot in OBJ and FBX.
    out.base_color_texture = texture_path(material, aiTextureType_BASE_COLOR, 0, notes, out.name);
    if (out.base_color_texture.empty()) {
        out.base_color_texture = texture_path(material, aiTextureType_DIFFUSE, 0, notes, out.name);
    }
    out.normal_texture = texture_path(material, aiTextureType_NORMALS, 0, notes, out.name);
    out.metallic_roughness_texture =
        texture_path(material, aiTextureType_UNKNOWN, 0, notes, out.name);
    if (out.metallic_roughness_texture.empty()) {
        out.metallic_roughness_texture = texture_path(material, aiTextureType_METALNESS, 0, notes, out.name);
    }
    out.emissive_texture = texture_path(material, aiTextureType_EMISSIVE, 0, notes, out.name);
    return out;
}

// Tangents for a mesh without them, from the engine's UVs. Each triangle gives the direction of increasing u (T) and of
// increasing v (B). Those are summed per vertex and made orthogonal to the normal. The handedness is the sign that relates
// B to cross(normal, T), so bitangent = w * cross(normal, tangent) follows increasing engine v.
std::vector<glm::vec4> generate_tangents(const ModelMesh& mesh, const std::vector<std::uint32_t>& triangles) {
    const std::size_t count = mesh.positions.size();
    std::vector<glm::vec3> tangent_sum(count, glm::vec3(0.0f));
    std::vector<glm::vec3> bitangent_sum(count, glm::vec3(0.0f));
    for (std::size_t f = 0; f + 2 < triangles.size(); f += 3) {
        const std::uint32_t a = triangles[f];
        const std::uint32_t b = triangles[f + 1];
        const std::uint32_t c = triangles[f + 2];
        const glm::vec3 e1 = mesh.positions[b] - mesh.positions[a];
        const glm::vec3 e2 = mesh.positions[c] - mesh.positions[a];
        const glm::vec2 d1 = mesh.uvs[b] - mesh.uvs[a];
        const glm::vec2 d2 = mesh.uvs[c] - mesh.uvs[a];
        const float determinant = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(determinant) < 1e-12f) {
            continue;  // degenerate UVs give no direction
        }
        const float inverse = 1.0f / determinant;
        const glm::vec3 t = (e1 * d2.y - e2 * d1.y) * inverse;
        const glm::vec3 bi = (e2 * d1.x - e1 * d2.x) * inverse;
        for (const std::uint32_t vertex : {a, b, c}) {
            tangent_sum[vertex] += t;
            bitangent_sum[vertex] += bi;
        }
    }

    std::vector<glm::vec4> tangents(count);
    for (std::size_t i = 0; i < count; ++i) {
        glm::vec3 n = mesh.normals[i];
        n = glm::dot(n, n) > 0.0f ? glm::normalize(n) : glm::vec3(0.0f, 0.0f, 1.0f);
        glm::vec3 t = tangent_sum[i] - n * glm::dot(n, tangent_sum[i]);
        if (glm::dot(t, t) > 1e-20f) {
            t = glm::normalize(t);
        } else {
            // No usable direction: any unit vector perpendicular to the normal.
            const glm::vec3 axis = std::fabs(n.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            t = glm::normalize(glm::cross(axis, n));
        }
        const float w = glm::dot(glm::cross(n, t), bitangent_sum[i]) < 0.0f ? -1.0f : 1.0f;
        tangents[i] = glm::vec4(t, w);
    }
    return tangents;
}

// The skeleton of the model: every node that a mesh names as a bone, in node order (parents first), so a joint index is the position of
// the bone among them. A joint's parent is its nearest ancestor that is also a joint.
struct SkeletonSource {
    std::vector<ModelJoint> joints;
    std::unordered_map<std::string, std::uint32_t> index_of;
};

void collect_joints(const aiNode& node, std::int32_t& node_index, std::int32_t parent_joint,
                    const std::unordered_map<std::string, glm::mat4>& offsets, SkeletonSource& skeleton) {
    const std::int32_t self_node = node_index++;
    std::int32_t joint_for_children = parent_joint;
    const std::string name = node.mName.C_Str();
    const auto offset = offsets.find(name);
    if (offset != offsets.end() && skeleton.index_of.count(name) == 0) {
        ModelJoint joint;
        joint.name = name;
        joint.parent = parent_joint;
        joint.node = self_node;
        joint.inverse_bind = offset->second;
        joint_for_children = static_cast<std::int32_t>(skeleton.joints.size());
        skeleton.index_of.emplace(name, static_cast<std::uint32_t>(skeleton.joints.size()));
        skeleton.joints.push_back(std::move(joint));
    }
    for (unsigned int c = 0; c < node.mNumChildren; ++c) {
        collect_joints(*node.mChildren[c], node_index, joint_for_children, offsets, skeleton);
    }
}

SkeletonSource build_skeleton(const aiScene& scene) {
    std::unordered_map<std::string, glm::mat4> offsets;
    for (unsigned int m = 0; m < scene.mNumMeshes; ++m) {
        const aiMesh& mesh = *scene.mMeshes[m];
        for (unsigned int b = 0; b < mesh.mNumBones; ++b) {
            offsets.emplace(mesh.mBones[b]->mName.C_Str(), to_glm(mesh.mBones[b]->mOffsetMatrix));
        }
    }
    SkeletonSource skeleton;
    if (!offsets.empty()) {
        std::int32_t node_index = 0;
        collect_joints(*scene.mRootNode, node_index, -1, offsets, skeleton);
    }
    return skeleton;
}

// Keeps the four strongest influences of each vertex and scales them to sum to one. A vertex with no influence follows joint 0.
void fill_skin(const aiMesh& mesh, const SkeletonSource& skeleton, ModelMesh& out) {
    struct Influence {
        std::uint32_t joint;
        float weight;
    };
    std::vector<std::vector<Influence>> per_vertex(mesh.mNumVertices);
    for (unsigned int b = 0; b < mesh.mNumBones; ++b) {
        const aiBone& bone = *mesh.mBones[b];
        const auto joint = skeleton.index_of.find(bone.mName.C_Str());
        if (joint == skeleton.index_of.end()) {
            continue;
        }
        for (unsigned int w = 0; w < bone.mNumWeights; ++w) {
            const aiVertexWeight& weight = bone.mWeights[w];
            if (weight.mVertexId < per_vertex.size() && weight.mWeight > 0.0f) {
                per_vertex[weight.mVertexId].push_back({joint->second, weight.mWeight});
            }
        }
    }
    out.joints.assign(mesh.mNumVertices, glm::uvec4(0));
    out.weights.assign(mesh.mNumVertices, glm::vec4(0.0f));
    for (unsigned int v = 0; v < mesh.mNumVertices; ++v) {
        auto& list = per_vertex[v];
        std::sort(list.begin(), list.end(), [](const Influence& a, const Influence& b) { return a.weight > b.weight; });
        float total = 0.0f;
        for (std::size_t k = 0; k < list.size() && k < 4; ++k) {
            total += list[k].weight;
        }
        if (total <= 0.0f) {
            out.weights[v] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            continue;
        }
        for (std::size_t k = 0; k < list.size() && k < 4; ++k) {
            out.joints[v][static_cast<int>(k)] = list[k].joint;
            out.weights[v][static_cast<int>(k)] = list[k].weight / total;
        }
    }
}

// Converts every animation whose channels name a joint. Times become seconds. Channels for nodes that are not joints are reported.
void convert_clips(const aiScene& scene, const SkeletonSource& skeleton, ModelPayload& model, std::vector<std::string>& notes) {
    for (unsigned int a = 0; a < scene.mNumAnimations; ++a) {
        const aiAnimation& animation = *scene.mAnimations[a];
        const double ticks_per_second = animation.mTicksPerSecond > 0.0 ? animation.mTicksPerSecond : 25.0;
        ModelClip clip;
        clip.name = animation.mName.length > 0 ? std::string(animation.mName.C_Str()) : "clip" + std::to_string(a);
        float last_time = 0.0f;
        std::size_t skipped = 0;
        for (unsigned int c = 0; c < animation.mNumChannels; ++c) {
            const aiNodeAnim& source = *animation.mChannels[c];
            const auto joint = skeleton.index_of.find(source.mNodeName.C_Str());
            if (joint == skeleton.index_of.end()) {
                ++skipped;
                continue;
            }
            ModelChannel channel;
            channel.joint = joint->second;
            const auto seconds = [ticks_per_second](double ticks) { return static_cast<float>(ticks / ticks_per_second); };
            for (unsigned int k = 0; k < source.mNumPositionKeys; ++k) {
                const aiVectorKey& key = source.mPositionKeys[k];
                channel.translation.push_back({seconds(key.mTime), glm::vec3(key.mValue.x, key.mValue.y, key.mValue.z)});
            }
            for (unsigned int k = 0; k < source.mNumRotationKeys; ++k) {
                const aiQuatKey& key = source.mRotationKeys[k];
                channel.rotation.push_back({seconds(key.mTime), glm::vec4(key.mValue.x, key.mValue.y, key.mValue.z, key.mValue.w)});
            }
            for (unsigned int k = 0; k < source.mNumScalingKeys; ++k) {
                const aiVectorKey& key = source.mScalingKeys[k];
                channel.scale.push_back({seconds(key.mTime), glm::vec3(key.mValue.x, key.mValue.y, key.mValue.z)});
            }
            for (const auto& key : channel.translation) {
                last_time = std::max(last_time, key.time);
            }
            for (const auto& key : channel.rotation) {
                last_time = std::max(last_time, key.time);
            }
            for (const auto& key : channel.scale) {
                last_time = std::max(last_time, key.time);
            }
            clip.channels.push_back(std::move(channel));
        }
        clip.duration = std::max(static_cast<float>(animation.mDuration / ticks_per_second), last_time);
        if (!(clip.duration > 0.0f)) {
            notes.push_back("animation '" + clip.name + "' has no duration and is skipped");
            continue;
        }
        if (skipped > 0) {
            notes.push_back("animation '" + clip.name + "' has " + std::to_string(skipped) +
                            " channel(s) for nodes that are not skinned joints; they are skipped");
        }
        model.clips.push_back(std::move(clip));
    }
}

// Converts one Assimp mesh. Its skinning, extra UV sets, colors, morph targets, and non-triangle faces are reported and
// left out. A mesh with no triangles, or none left after cooking, is dropped, and the function returns nothing for it.
// Cooking (US-023) runs last, so the tangents above are computed from the source triangles.
Result<std::optional<ModelMesh>, std::string> convert_mesh(const aiMesh& mesh, unsigned int index, Format format,
                                                           const SkeletonSource& skeleton, std::vector<std::string>& notes) {
    const std::string label = label_for(mesh, index);
    if (mesh.mVertices == nullptr || mesh.mNumVertices == 0) {
        notes.push_back(label + " has no vertices and is skipped");
        return success(std::optional<ModelMesh>());
    }

    ModelMesh out;
    out.name = mesh.mName.C_Str();
    out.material = static_cast<std::int32_t>(mesh.mMaterialIndex);

    const unsigned int vertex_count = mesh.mNumVertices;
    out.positions.resize(vertex_count);
    for (unsigned int i = 0; i < vertex_count; ++i) {
        const aiVector3D& p = mesh.mVertices[i];
        out.positions[i] = glm::vec3(p.x, p.y, p.z);
    }
    if (mesh.HasNormals()) {
        out.normals.resize(vertex_count);
        for (unsigned int i = 0; i < vertex_count; ++i) {
            const aiVector3D& n = mesh.mNormals[i];
            out.normals[i] = glm::vec3(n.x, n.y, n.z);
        }
    }
    if (mesh.HasTextureCoords(0)) {
        out.uvs.resize(vertex_count);
        for (unsigned int i = 0; i < vertex_count; ++i) {
            const aiVector3D& uv = mesh.mTextureCoords[0][i];
            out.uvs[i] = glm::vec2(uv.x, 1.0f - uv.y);
        }
    }

    std::size_t skipped_faces = 0;
    std::vector<std::uint32_t> triangles;
    triangles.reserve(static_cast<std::size_t>(mesh.mNumFaces) * 3);
    for (unsigned int f = 0; f < mesh.mNumFaces; ++f) {
        const aiFace& face = mesh.mFaces[f];
        if (face.mNumIndices != 3) {
            ++skipped_faces;
            continue;
        }
        for (unsigned int k = 0; k < 3; ++k) {
            triangles.push_back(face.mIndices[k]);
        }
    }
    if (skipped_faces > 0) {
        notes.push_back(label + " has " + std::to_string(skipped_faces) + " point or line face(s); they are skipped");
    }
    if (triangles.empty()) {
        notes.push_back(label + " has no triangles and is skipped");
        return success(std::optional<ModelMesh>());
    }

    // glTF tangents that the file provides are in the engine's UV space. Assimp reports the bitangent of each one as
    // cross(normal, tangent) * w, so w is the sign of that bitangent. Other tangents are computed from the UVs.
    if (format == Format::Gltf && mesh.mTangents != nullptr && mesh.HasNormals()) {
        out.tangents.resize(vertex_count);
        for (unsigned int i = 0; i < vertex_count; ++i) {
            const aiVector3D& t = mesh.mTangents[i];
            float w = 1.0f;
            if (mesh.mBitangents != nullptr) {
                w = ((mesh.mNormals[i] ^ t) * mesh.mBitangents[i]) < 0.0f ? -1.0f : 1.0f;
            }
            out.tangents[i] = glm::vec4(t.x, t.y, t.z, w);
        }
    } else if (!out.uvs.empty() && !out.normals.empty()) {
        out.tangents = generate_tangents(out, triangles);
    }

    if (mesh.GetNumUVChannels() > 1) {
        notes.push_back(label + " has " + std::to_string(mesh.GetNumUVChannels() - 1) +
                        " extra UV set(s); only set 0 is imported");
    }
    if (mesh.GetNumColorChannels() > 0) {
        notes.push_back(label + " has vertex colors; they are not imported yet");
    }
    if (mesh.HasBones() && !skeleton.joints.empty()) {
        fill_skin(mesh, skeleton, out);
    }
    if (mesh.mNumAnimMeshes > 0) {
        notes.push_back(label + " has " + std::to_string(mesh.mNumAnimMeshes) + " morph target(s); they are not imported");
    }

    auto cooked = cook_mesh(out, std::move(triangles));
    if (!cooked.ok()) {
        return failure(label + " could not be cooked: " + cooked.error());
    }
    if (cooked.value().degenerate_triangles > 0) {
        notes.push_back(label + " has " + std::to_string(cooked.value().degenerate_triangles) +
                        " triangle(s) that repeat a vertex; they are dropped");
    }
    if (out.lods[0].indices.empty()) {
        notes.push_back(label + " has no triangles after its degenerate triangles are dropped and is skipped");
        return success(std::optional<ModelMesh>());
    }
    return success(std::optional<ModelMesh>(std::move(out)));
}

// Pre-order walk, so every parent is written before its children. Nodes whose meshes were dropped keep their place in
// the hierarchy and simply have no mesh references.
void add_node(const aiNode& node, std::int32_t parent, const glm::mat4& root_conversion,
              const std::vector<std::int32_t>& mesh_map, ModelPayload& model, bool is_root) {
    ModelNode out;
    out.name = node.mName.C_Str();
    out.parent = parent;
    out.local = is_root ? root_conversion * to_glm(node.mTransformation) : to_glm(node.mTransformation);
    for (unsigned int i = 0; i < node.mNumMeshes; ++i) {
        const std::int32_t mapped = mesh_map[node.mMeshes[i]];
        if (mapped >= 0) {
            out.meshes.push_back(static_cast<std::uint32_t>(mapped));
        }
    }
    const auto self = static_cast<std::int32_t>(model.nodes.size());
    model.nodes.push_back(std::move(out));
    for (unsigned int c = 0; c < node.mNumChildren; ++c) {
        add_node(*node.mChildren[c], self, root_conversion, mesh_map, model, false);
    }
}

Result<ImportOutput, std::string> import_model(std::string_view source, double scale) {
    const Format format = detect_format(source);
    ImportOutput output;

    Assimp::Importer importer;
    importer.SetIOHandler(new RefuseExternalFiles(output.unsupported));  // the importer takes ownership
    const aiScene* scene = importer.ReadFileFromMemory(source.data(), source.size(), kPostprocessFlags, hint_for(format));
    if (scene == nullptr || scene->mRootNode == nullptr) {
        return failure("the model could not be read: " + std::string(importer.GetErrorString()));
    }

    ModelPayload model;
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
        model.materials.push_back(convert_material(*scene->mMaterials[i], format, output.unsupported));
    }

    const SkeletonSource skeleton = build_skeleton(*scene);
    std::vector<std::int32_t> mesh_map(scene->mNumMeshes, -1);
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        auto converted = convert_mesh(*scene->mMeshes[i], i, format, skeleton, output.unsupported);
        if (!converted.ok()) {
            return failure(converted.error());
        }
        if (converted.value().has_value()) {
            mesh_map[i] = static_cast<std::int32_t>(model.meshes.size());
            model.meshes.push_back(std::move(converted).value().value());
        }
    }

    if (scene->mNumCameras > 0) {
        output.unsupported.push_back(std::to_string(scene->mNumCameras) + " camera(s) are not imported");
    }
    if (scene->mNumLights > 0) {
        output.unsupported.push_back(std::to_string(scene->mNumLights) + " light(s) are not imported");
    }

    // Assimp's FBX root transform does not include the file's unit scale, so the importer applies it here.
    double unit_factor = 1.0;
    if (format == Format::Fbx) {
        // Assimp stores this value as a float, and a metadata lookup fails on a type mismatch, so the type must match.
        float unit_scale = static_cast<float>(kFbxUnitScaleDefault);
        if (scene->mMetaData != nullptr) {
            scene->mMetaData->Get(std::string("UnitScaleFactor"), unit_scale);
        }
        unit_factor = static_cast<double>(unit_scale) / kFbxCentimetersPerMeter;
    }
    const auto factor = static_cast<float>(unit_factor * scale);
    add_node(*scene->mRootNode, -1, glm::scale(glm::mat4(1.0f), glm::vec3(factor)), mesh_map, model, true);
    // The skeleton and the clips (US-022). Joint nodes were numbered in the same pre-order that add_node used.
    model.joints = skeleton.joints;
    if (!skeleton.joints.empty()) {
        convert_clips(*scene, skeleton, model, output.unsupported);
    } else if (scene->mNumAnimations > 0) {
        output.unsupported.push_back(std::to_string(scene->mNumAnimations) + " animation(s) animate no skinned joint and are not imported");
    }

    // A source Assimp accepts but that has no geometry (plain text, for example) is not a model.
    if (model.meshes.empty()) {
        return failure(std::string("the source contains no triangle meshes"));
    }
    if (auto valid = validate_model(model); !valid.ok()) {
        return failure("the converted model is not valid: " + valid.error());
    }
    output.payload = write_model_payload(model);
    return success(std::move(output));
}

// Settings: {"scale": <number>}, a multiplier applied to the whole model, default 1. It must be finite and greater than zero.
class ModelImporter final : public Importer {
public:
    [[nodiscard]] std::string_view type() const noexcept override {
        return "model";
    }
    [[nodiscard]] int version() const noexcept override {
        return 3;  // US-022: skeleton, skin weights, and clips
    }

    [[nodiscard]] Result<std::string, std::string> canonical_settings(std::string_view settings_json) const override {
        auto parsed = parse_settings(settings_json);
        if (!parsed.ok()) {
            return failure(parsed.error());
        }
        Json canonical = Json::object();
        canonical["scale"] = parsed.value();
        return success(canonical.dump());
    }

    [[nodiscard]] Result<ImportOutput, std::string> import(std::string_view source, std::string_view settings) const override {
        auto scale = parse_settings(settings);
        if (!scale.ok()) {
            return failure(scale.error());
        }
        return import_model(source, scale.value());
    }

private:
    static Result<double, std::string> parse_settings(std::string_view settings_json) {
        Json settings = Json::parse(std::string(settings_json), nullptr, false);
        if (settings.is_discarded() || !settings.is_object()) {
            return failure(std::string("settings must be a JSON object"));
        }
        double scale = 1.0;
        for (auto it = settings.begin(); it != settings.end(); ++it) {
            if (it.key() != "scale") {
                return failure("unknown setting '" + it.key() + "'");
            }
            if (!it->is_number()) {
                return failure(std::string("setting 'scale' must be a number"));
            }
            scale = it->get<double>();
        }
        if (!std::isfinite(scale) || scale <= 0.0) {
            return failure(std::string("setting 'scale' must be a finite number greater than zero"));
        }
        return success(scale);
    }
};

}  // namespace

std::unique_ptr<Importer> make_model_importer() {
    return std::make_unique<ModelImporter>();
}

}  // namespace engine::converter
