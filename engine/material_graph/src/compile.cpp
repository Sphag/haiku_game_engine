#include "engine/material_graph/compile.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

namespace engine::material_graph {
namespace {

constexpr std::string_view kEntryPoint = "PSGraph";
constexpr std::string_view kProfile = "ps_6_0";

shader::Diagnostic error_at(const std::string& file, std::string message) {
    shader::Diagnostic diagnostic;
    diagnostic.severity = shader::Severity::Error;
    diagnostic.file = file;
    diagnostic.message = std::move(message);
    return diagnostic;
}

// Writes the source as <dir>/<hash>.hlsl. The name is the content hash, so a file that already exists is compared and reused when it
// holds the same bytes, and rewritten when it does not.
core::Result<std::string, shader::Diagnostic> write_source(const GeneratedShader& shader, const std::string& dir) {
    const std::filesystem::path path = std::filesystem::path(dir) / (shader.hash + ".hlsl");
    const std::string file = path.string();
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in && existing == shader.hlsl) {
            return core::success(file);
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << shader.hlsl;
    out.close();
    if (!out) {
        return core::failure(error_at(file, "cannot write the generated source"));
    }
    return core::success(file);
}

// The checks that tie the compiled block and bindings to the generated layout. Each uses the reflection the target reports: a block
// the compiler dropped because no variant reads it is not an error, because nothing in it is bound.
std::vector<shader::Diagnostic> check_reflection(const std::string& source, const GeneratedShader& shader,
                                                 const shader::ShaderReflection& reflection) {
    std::vector<shader::Diagnostic> checks;

    shader::CpuBlockLayout block;
    block.name = std::string(kParameterBlockName);
    block.set = kMaterialSet;
    block.binding = 0;
    block.size_bytes = shader.parameters.size_bytes();
    for (const ParameterEntry& entry : shader.parameters.entries) {
        shader::ReflectedMember member;
        member.name = "p_" + entry.name;
        member.offset = 16u * entry.row;
        block.members.push_back(member);
    }
    for (const shader::ReflectedBlock& reflected : reflection.blocks) {
        if (reflected.name == block.name) {
            auto block_checks = shader::validate_cpu_layouts(source, reflection, {block});
            checks.insert(checks.end(), block_checks.begin(), block_checks.end());
            break;
        }
    }

    // Set 1: the parameter block at binding 0 and one shader resource per texture slot from kFirstTextureBinding on.
    rhi::DescriptorSetLayoutDesc material_set;
    rhi::DescriptorBindingDesc params;
    params.binding = 0;
    params.type = rhi::DescriptorType::ConstantBuffer;
    params.visibility = rhi::ShaderStage::Pixel;
    material_set.bindings.push_back(params);
    for (std::size_t i = 0; i < shader.textures.size(); ++i) {
        rhi::DescriptorBindingDesc texture;
        texture.binding = kFirstTextureBinding + static_cast<std::uint32_t>(i);
        texture.type = rhi::DescriptorType::ShaderResource;
        texture.visibility = rhi::ShaderStage::Pixel;
        material_set.bindings.push_back(texture);
    }
    std::vector<rhi::DescriptorSetLayoutDesc> layouts(kMaterialSet + 1);
    layouts[kMaterialSet] = material_set;
    auto binding_checks = shader::validate_bindings(source, reflection, layouts);
    checks.insert(checks.end(), binding_checks.begin(), binding_checks.end());
    return checks;
}

shader::CompileRequest request_for(const std::string& source_path, const MaterialVariant& variant, shader::ShaderTarget target) {
    shader::CompileRequest request;
    request.source_path = source_path;
    request.entry_point = std::string(kEntryPoint);
    request.profile = std::string(kProfile);
    request.target = target;
    request.defines.push_back(shader::ShaderDefine{"MG_BLEND_MODE", std::to_string(variant.blend_mode)});
    request.defines.push_back(shader::ShaderDefine{"MG_TWO_SIDED", variant.two_sided ? "1" : "0"});
    return request;
}

}  // namespace

std::string MaterialVariant::name() const {
    std::string blend = "opaque";
    if (blend_mode == 1) {
        blend = "alpha_mask";
    } else if (blend_mode == 2) {
        blend = "alpha_blend";
    }
    return two_sided ? blend + "_two_sided" : blend;
}

std::vector<MaterialVariant> material_variants() {
    std::vector<MaterialVariant> variants;
    for (std::uint32_t blend = 0; blend < 3; ++blend) {
        for (const bool two_sided : {false, true}) {
            MaterialVariant variant;
            variant.blend_mode = blend;
            variant.two_sided = two_sided;
            variants.push_back(variant);
        }
    }
    return variants;
}

const VariantBinary* CompiledMaterial::find(const MaterialVariant& variant, shader::ShaderTarget target) const {
    for (const VariantBinary& binary : binaries) {
        if (binary.variant.blend_mode == variant.blend_mode && binary.variant.two_sided == variant.two_sided && binary.target == target) {
            return &binary;
        }
    }
    return nullptr;
}

std::string CompileFailure::describe() const {
    std::string text;
    for (const GraphError& error : graph_errors) {
        text += to_text(error) + "\n";
    }
    text += shader::diagnostics_to_text(shader_diagnostics);
    return text;
}

core::Result<CompiledMaterial, CompileFailure> compile_material(const Graph& graph, const shader::ShaderCompiler& compiler,
                                                                shader::PipelineCache& cache, const std::string& generated_dir) {
    auto generated = generate_hlsl(graph);
    if (!generated.ok()) {
        return core::failure(CompileFailure{std::move(generated.error()), {}});
    }
    CompiledMaterial result;
    result.shader = std::move(generated).value();

    auto written = write_source(result.shader, generated_dir);
    if (!written.ok()) {
        return core::failure(CompileFailure{{}, {written.error()}});
    }
    result.source_path = written.value();

    constexpr std::array<shader::ShaderTarget, 2> kTargets = {shader::ShaderTarget::Dxil, shader::ShaderTarget::SpirV};
    for (const MaterialVariant& variant : material_variants()) {
        for (const shader::ShaderTarget target : kTargets) {
            const shader::CompileRequest request = request_for(result.source_path, variant, target);
            VariantBinary binary;
            binary.variant = variant;
            binary.target = target;
            binary.cache_key = shader::request_key(request, cache.environment());

            if (const shader::CacheEntry* cached = cache.find(binary.cache_key); cached != nullptr) {
                binary.from_cache = true;
                binary.bytecode = cached->bytecode;
                binary.reflection = cached->reflection;
            } else {
                shader::CompileResult compiled = compiler.compile(request);
                if (!compiled.ok) {
                    return core::failure(CompileFailure{{}, std::move(compiled.diagnostics)});
                }
                shader::CacheEntry fresh;
                fresh.request_key = binary.cache_key;
                fresh.bytecode = compiled.bytecode;
                fresh.reflection = compiled.reflection;
                bool stamped = true;
                for (const std::string& path : compiled.dependencies) {
                    auto hash = shader::hash_file(path);
                    if (!hash.ok()) {
                        stamped = false;
                        break;
                    }
                    fresh.dependencies.push_back(shader::CacheDependency{path, hash.value()});
                }
                if (stamped) {
                    cache.insert(fresh);
                }
                binary.bytecode = std::move(compiled.bytecode);
                binary.reflection = std::move(compiled.reflection);
            }

            auto checks = check_reflection(result.source_path, result.shader, binary.reflection);
            if (shader::has_errors(checks)) {
                return core::failure(CompileFailure{{}, std::move(checks)});
            }
            result.binaries.push_back(std::move(binary));
        }
    }
    return core::success(std::move(result));
}

}  // namespace engine::material_graph
