#pragma once

// Material compilation (US-067): the generated source of a graph, built for every material variant and both shader targets through
// the pinned compiler and the pipeline cache (US-031).
//
// Variants. A variant is a blend mode (opaque, alpha mask, alpha blend) and a sidedness. Each is a build-time define, so six variants
// share one source and differ only in the define set, which the request key includes. Six variants times two targets is twelve
// entries per graph.
//
// Cache keys. The source is stored under its own hash, so a topology change makes a new path and a new key. A parameter value
// change makes neither: the source is the same, and every entry is a cache hit.

#include "engine/core/result.hpp"
#include "engine/material_graph/codegen.hpp"
#include "engine/material_graph/graph.hpp"
#include "engine/shader/compiler.hpp"
#include "engine/shader/diagnostics.hpp"
#include "engine/shader/pipeline_cache.hpp"
#include "engine/shader/reflection.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace engine::material_graph {

struct MaterialVariant {
    std::uint32_t blend_mode = 0;  // 0 opaque, 1 alpha mask, 2 alpha blend (MG_BLEND_MODE)
    bool two_sided = false;        // MG_TWO_SIDED

    [[nodiscard]] std::string name() const;  // "opaque", "alpha_mask_two_sided", ...
};

// Every variant the engine builds, in a fixed order.
[[nodiscard]] std::vector<MaterialVariant> material_variants();

struct VariantBinary {
    MaterialVariant variant;
    shader::ShaderTarget target = shader::ShaderTarget::Dxil;
    std::string cache_key;  // the request key the entry is stored under
    bool from_cache = false;
    std::vector<std::uint8_t> bytecode;
    shader::ShaderReflection reflection;
};

struct CompiledMaterial {
    GeneratedShader shader;
    std::string source_path;  // the generated file, <generated_dir>/<shader.hash>.hlsl
    std::vector<VariantBinary> binaries;

    // The entry for one variant and target, or nullptr.
    [[nodiscard]] const VariantBinary* find(const MaterialVariant& variant, shader::ShaderTarget target) const;
};

struct CompileFailure {
    std::vector<GraphError> graph_errors;
    std::vector<shader::Diagnostic> shader_diagnostics;

    // The graph errors and the compiler diagnostics, one per line.
    [[nodiscard]] std::string describe() const;
};

// Generates the source into `generated_dir`, which must exist, and compiles every variant for DXIL and SPIR-V. An entry the cache
// holds for this request is reused; a fresh compile is stored with the hashes of the files it read. Fails with the graph's errors,
// the compiler's diagnostics, or a reflection that disagrees with the generated layout.
[[nodiscard]] core::Result<CompiledMaterial, CompileFailure> compile_material(const Graph& graph, const shader::ShaderCompiler& compiler,
                                                                             shader::PipelineCache& cache,
                                                                             const std::string& generated_dir);

}  // namespace engine::material_graph
