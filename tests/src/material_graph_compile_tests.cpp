// Material graph compilation through the pinned DXC (US-067). Every variant of a graph builds for DXIL and SPIR-V, a repeat compile and
// a parameter change are served from the pipeline cache, and a topology change builds again under new keys.

#include "material_graph_fixture.hpp"

#include "engine/material_graph/compile.hpp"
#include "engine/shader/compiler.hpp"
#include "engine/shader/pipeline_cache.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <system_error>

using engine::material_graph::CompiledMaterial;
using engine::material_graph::compile_material;
using engine::material_graph::Graph;
using engine::material_graph::material_variants;
using engine::material_graph::MaterialVariant;
using engine::material_graph::Node;
using engine::material_graph::NodeKind;
using engine::material_graph::tests::edit;
using engine::material_graph::tests::parse_valid;
using engine::material_graph::tests::sample_graph_json;
using engine::shader::BindingClass;
using engine::shader::PipelineCache;
using engine::shader::ShaderCompiler;
using engine::shader::ShaderTarget;

namespace fs = std::filesystem;

namespace {

// A fresh directory per test, so no test sees another's generated files or cache entries.
fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(ENGINE_TEST_SHADER_SCRATCH) / "material_graph" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "generated");
    fs::create_directories(dir / "compiler");
    return dir;
}

ShaderCompiler make_compiler(const fs::path& dir) {
    auto created = ShaderCompiler::create(ENGINE_TEST_DXC_PATH, (dir / "compiler").string());
    if (!created.ok()) {
        FAIL(created.error());
    }
    return std::move(created).value();
}

CompiledMaterial compile_or_fail(const Graph& graph, const ShaderCompiler& compiler, PipelineCache& cache, const fs::path& dir) {
    auto compiled = compile_material(graph, compiler, cache, (dir / "generated").string());
    if (!compiled.ok()) {
        FAIL(compiled.error().describe());
    }
    return std::move(compiled).value();
}

std::set<std::string> keys_of(const CompiledMaterial& material) {
    std::set<std::string> keys;
    for (const auto& binary : material.binaries) {
        keys.insert(binary.cache_key);
    }
    return keys;
}

bool every_entry_from_cache(const CompiledMaterial& material) {
    for (const auto& binary : material.binaries) {
        if (!binary.from_cache) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Every variant of a graph compiles for DXIL and SPIR-V, each in its target's container", "[integration][material_graph]") {
    const fs::path dir = scratch("every_variant");
    const ShaderCompiler compiler = make_compiler(dir);
    PipelineCache cache("material-graph-test-environment");
    const CompiledMaterial material = compile_or_fail(parse_valid(sample_graph_json()), compiler, cache, dir);

    REQUIRE(material.binaries.size() == 2 * material_variants().size());
    REQUIRE(fs::exists(material.source_path));
    REQUIRE(fs::path(material.source_path).filename().string() == material.shader.hash + ".hlsl");

    for (const MaterialVariant& variant : material_variants()) {
        const auto* dxil = material.find(variant, ShaderTarget::Dxil);
        const auto* spirv = material.find(variant, ShaderTarget::SpirV);
        REQUIRE(dxil != nullptr);
        REQUIRE(spirv != nullptr);
        REQUIRE(dxil->bytecode.size() > 4);
        REQUIRE(spirv->bytecode.size() > 4);
        // DXIL is a DXBC container; SPIR-V starts with its magic number, 0x07230203, little-endian.
        CHECK(std::string(dxil->bytecode.begin(), dxil->bytecode.begin() + 4) == "DXBC");
        const bool spirv_magic = spirv->bytecode[0] == 0x03 && spirv->bytecode[1] == 0x02 && spirv->bytecode[2] == 0x23 && spirv->bytecode[3] == 0x07;
        CHECK(spirv_magic);
        CHECK_FALSE(dxil->from_cache);
    }

    // The opaque DXIL entry reads both textures from set 1, bindings 1 and 2, as the layout says (binding 0 is the parameter block).
    const auto* opaque = material.find(MaterialVariant{0, false}, ShaderTarget::Dxil);
    REQUIRE(opaque != nullptr);
    bool albedo_bound = false;
    bool normal_bound = false;
    for (const auto& binding : opaque->reflection.bindings) {
        if (binding.set == 1 && binding.cls == BindingClass::ShaderResource && binding.binding == 1) {
            albedo_bound = true;
        }
        if (binding.set == 1 && binding.cls == BindingClass::ShaderResource && binding.binding == 2) {
            normal_bound = true;
        }
    }
    CHECK(albedo_bound);
    CHECK(normal_bound);
}

TEST_CASE("A repeat compile is served from the cache, and a parameter default change needs no rebuild", "[integration][material_graph]") {
    const fs::path dir = scratch("parameter_change");
    const ShaderCompiler compiler = make_compiler(dir);
    PipelineCache cache("material-graph-test-environment");

    const CompiledMaterial first = compile_or_fail(parse_valid(sample_graph_json()), compiler, cache, dir);
    const CompiledMaterial repeat = compile_or_fail(parse_valid(sample_graph_json()), compiler, cache, dir);
    CHECK(every_entry_from_cache(repeat));
    CHECK(keys_of(repeat) == keys_of(first));

    // The tint default is not in the source, so the file, the keys, and the bytecode are unchanged.
    const std::string retinted = edit(sample_graph_json(), "\"value\": [1, 1, 1, 1]", "\"value\": [0.5, 0.25, 0.75, 1]");
    const CompiledMaterial changed = compile_or_fail(parse_valid(retinted), compiler, cache, dir);
    CHECK(changed.shader.hash == first.shader.hash);
    CHECK(changed.source_path == first.source_path);
    CHECK(every_entry_from_cache(changed));
    CHECK(keys_of(changed) == keys_of(first));
}

TEST_CASE("A topology or constant change builds new entries under new keys", "[integration][material_graph]") {
    const fs::path dir = scratch("topology_change");
    const ShaderCompiler compiler = make_compiler(dir);
    PipelineCache cache("material-graph-test-environment");
    const CompiledMaterial first = compile_or_fail(parse_valid(sample_graph_json()), compiler, cache, dir);

    // The tinted node now adds the tint instead of multiplying it: a different source.
    const std::string rewired = edit(sample_graph_json(), "\"op\": \"multiply\"", "\"op\": \"add\"");
    const CompiledMaterial changed = compile_or_fail(parse_valid(rewired), compiler, cache, dir);
    CHECK(changed.shader.hash != first.shader.hash);
    CHECK(changed.source_path != first.source_path);
    CHECK_FALSE(changed.binaries.front().from_cache);

    // No key of the first graph is reused by the second.
    std::set<std::string> overlap;
    for (const std::string& key : keys_of(changed)) {
        if (keys_of(first).contains(key)) {
            overlap.insert(key);
        }
    }
    CHECK(overlap.empty());

    // Going back to the first graph is served from the cache entries it left behind.
    const CompiledMaterial back = compile_or_fail(parse_valid(sample_graph_json()), compiler, cache, dir);
    CHECK(every_entry_from_cache(back));
    CHECK(keys_of(back) == keys_of(first));
}

TEST_CASE("A graph with errors fails before the compiler runs, and no source file is written", "[integration][material_graph]") {
    const fs::path dir = scratch("graph_errors");
    const ShaderCompiler compiler = make_compiler(dir);
    PipelineCache cache("material-graph-test-environment");

    Graph graph = parse_valid(sample_graph_json());
    Node broken;
    broken.id = "broken";
    broken.kind = NodeKind::Arithmetic;
    graph.nodes.push_back(broken);

    auto compiled = compile_material(graph, compiler, cache, (dir / "generated").string());
    REQUIRE_FALSE(compiled.ok());
    REQUIRE_FALSE(compiled.error().graph_errors.empty());
    CHECK(compiled.error().describe().find("node 'broken' (arithmetic): input 'a' is not connected") != std::string::npos);
    CHECK(compiled.error().shader_diagnostics.empty());
    CHECK(cache.size() == 0);
    CHECK(fs::is_empty(dir / "generated"));
}
