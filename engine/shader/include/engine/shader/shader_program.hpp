#pragma once

// A compute shader that can be reloaded while the game runs (US-031). A reload compiles or reuses the cached bytecode, checks it
// against the declared layouts, and creates a new pipeline. The old pipeline is retired only after the new one exists. A failed
// reload at any step keeps the last valid pipeline current and returns its diagnostics.

#include "engine/rhi/device.hpp"
#include "engine/shader/compiler.hpp"
#include "engine/shader/diagnostics.hpp"
#include "engine/shader/pipeline_cache.hpp"
#include "engine/shader/reflection.hpp"

#include <string>
#include <vector>

namespace engine::shader {

struct ShaderProgramDesc {
    std::string debug_name;
    CompileRequest request;  // the profile must name a compute stage (cs_*)
    std::vector<rhi::DescriptorSetLayoutDesc> layouts;             // the declared layout of each set
    std::vector<rhi::DescriptorSetLayoutHandle> layout_handles;    // the device objects for those layouts, in set order
    std::vector<CpuBlockLayout> cpu_blocks;                        // the CPU view of every constant buffer the shader reads
};

struct ReloadReport {
    bool replaced = false;    // the pipeline built by this reload is now current
    bool from_cache = false;  // the bytecode came from the pipeline cache rather than the compiler
    std::vector<Diagnostic> diagnostics;  // errors and warnings from this reload; empty when it is clean
};

class ShaderProgram {
public:
    // The compiler and the cache are borrowed and must outlive the program.
    ShaderProgram(ShaderProgramDesc desc, const ShaderCompiler& compiler, PipelineCache& cache);

    // Brings the pipeline up to date with the source on disk. On failure the previous pipeline stays current.
    ReloadReport reload(rhi::Device& device);

    // The current pipeline, or an invalid handle before the first successful reload or after release().
    [[nodiscard]] rhi::PipelineHandle pipeline() const noexcept {
        return pipeline_;
    }

    // Diagnostics from the most recent reload, for the editor and the log.
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }
    // The same diagnostics as the machine-readable JSON report (see diagnostics_to_json).
    [[nodiscard]] std::string diagnostics_json() const;

    // Destroys the current pipeline. The device frees it once the work that uses it has completed.
    rhi::RhiStatus release(rhi::Device& device);

private:
    ShaderProgramDesc desc_;
    const ShaderCompiler* compiler_;
    PipelineCache* cache_;
    rhi::PipelineHandle pipeline_{};
    std::vector<Diagnostic> diagnostics_;
};

}  // namespace engine::shader
