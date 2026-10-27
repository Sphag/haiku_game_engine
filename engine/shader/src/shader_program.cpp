#include "engine/shader/shader_program.hpp"

#include <iterator>
#include <utility>

namespace engine::shader {
namespace {

void add_error(std::vector<Diagnostic>& diagnostics, const std::string& file, std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.file = file;
    diagnostic.message = std::move(message);
    diagnostics.push_back(std::move(diagnostic));
}

void append(std::vector<Diagnostic>& target, std::vector<Diagnostic> more) {
    target.insert(target.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
}

}  // namespace

ShaderProgram::ShaderProgram(ShaderProgramDesc desc, const ShaderCompiler& compiler, PipelineCache& cache)
    : desc_(std::move(desc)), compiler_(&compiler), cache_(&cache) {}

ReloadReport ShaderProgram::reload(rhi::Device& device) {
    ReloadReport report;
    const std::string& source = desc_.request.source_path;
    // Every early return keeps pipeline_ as it is, so the last valid pipeline stays current.
    const auto finish = [this, &report]() {
        diagnostics_ = report.diagnostics;
        return report;
    };

    if (desc_.request.profile.rfind("cs_", 0) != 0) {
        add_error(report.diagnostics, source,
                  "ShaderProgram builds compute pipelines; profile '" + desc_.request.profile +
                      "' is not a compute profile. Graphics and mesh pipelines are not built here yet.");
        return finish();
    }
    const bool wants_dxil = desc_.request.target == ShaderTarget::Dxil;
    if (device.backend() == rhi::Backend::Dx12 && !wants_dxil) {
        add_error(report.diagnostics, source, "the DX12 device needs DXIL, but the request targets SPIR-V");
        return finish();
    }
    if (device.backend() == rhi::Backend::Vulkan && wants_dxil) {
        add_error(report.diagnostics, source, "the Vulkan device needs SPIR-V, but the request targets DXIL");
        return finish();
    }

    // Cached bytecode is used only while every dependency still hashes to its stamp (PipelineCache::find).
    std::vector<std::uint8_t> bytecode;
    ShaderReflection reflection;
    const std::string key = request_key(desc_.request, cache_->environment());
    if (const CacheEntry* cached = cache_->find(key); cached != nullptr) {
        bytecode = cached->bytecode;
        reflection = cached->reflection;
        report.from_cache = true;
    } else {
        CompileResult compiled = compiler_->compile(desc_.request);
        append(report.diagnostics, compiled.diagnostics);
        if (!compiled.ok) {
            return finish();
        }

        CacheEntry fresh;
        fresh.request_key = key;
        fresh.bytecode = compiled.bytecode;
        fresh.reflection = compiled.reflection;
        bool stamped = true;
        for (const std::string& path : compiled.dependencies) {
            auto hash = hash_file(path);
            if (!hash.ok()) {
                stamped = false;
                break;
            }
            fresh.dependencies.push_back(CacheDependency{path, hash.value()});
        }
        if (stamped) {
            cache_->insert(fresh);
        }
        bytecode = std::move(compiled.bytecode);
        reflection = std::move(compiled.reflection);
    }

    // A reflection that disagrees with the declared layout is a failed reload, even though the compile succeeded.
    std::vector<Diagnostic> checks = validate_bindings(source, reflection, desc_.layouts);
    append(checks, validate_cpu_layouts(source, reflection, desc_.cpu_blocks));
    const bool layout_ok = !has_errors(checks);
    append(report.diagnostics, std::move(checks));
    if (!layout_ok) {
        return finish();
    }

    rhi::PipelineDesc pipeline_desc;
    pipeline_desc.kind = rhi::PipelineKind::Compute;
    pipeline_desc.shaders.push_back(rhi::ShaderBinary{rhi::ShaderStage::Compute, desc_.request.entry_point, std::move(bytecode)});
    pipeline_desc.set_layouts = desc_.layout_handles;
    pipeline_desc.debug_name = desc_.debug_name;
    auto created = device.create_pipeline(pipeline_desc);
    if (!created.ok()) {
        add_error(report.diagnostics, source, created.error().message);
        return finish();
    }

    // Only now is the old pipeline retired. The device frees it once the work that uses it has completed.
    if (pipeline_.valid()) {
        const rhi::RhiStatus retired = device.destroy(pipeline_);
        if (!retired.ok()) {
            add_error(report.diagnostics, source, retired.error().message);
        }
    }
    pipeline_ = std::move(created).value();
    report.replaced = true;
    return finish();
}

std::string ShaderProgram::diagnostics_json() const {
    return diagnostics_to_json(desc_.request.source_path, diagnostics_);
}

rhi::RhiStatus ShaderProgram::release(rhi::Device& device) {
    if (!pipeline_.valid()) {
        return core::success();
    }
    const rhi::RhiStatus status = device.destroy(pipeline_);
    pipeline_ = rhi::PipelineHandle{};
    return status;
}

}  // namespace engine::shader
