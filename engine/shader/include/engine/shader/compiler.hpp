#pragma once

// HLSL compilation through the pinned DirectXShaderCompiler (US-003). The compiler is always the executable the build
// staged; it is never looked up on PATH. Each compile writes its listing and dependency file to a scratch directory, and
// the results come back as bytecode, reflection, diagnostics, and the list of files that were read.

#include "engine/core/result.hpp"
#include "engine/shader/diagnostics.hpp"
#include "engine/shader/reflection.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace engine::shader {

struct ShaderDefine {
    std::string name;
    std::string value;  // "" defines the name as empty
};

struct CompileRequest {
    std::string source_path;
    std::string entry_point;
    std::string profile;  // for example "cs_6_0"; the stage is the profile's prefix
    ShaderTarget target = ShaderTarget::Dxil;
    std::vector<std::string> include_dirs;
    std::vector<ShaderDefine> defines;
};

struct CompileResult {
    bool ok = false;
    std::vector<std::uint8_t> bytecode;  // DXIL or SPIR-V, as the target says; empty on failure
    ShaderReflection reflection;         // empty on failure
    std::vector<Diagnostic> diagnostics; // errors and warnings, in the order the compiler printed them
    std::vector<std::string> dependencies;  // the source first, then every include the compile read
};

class ShaderCompiler {
public:
    // dxc_path is the pinned executable. scratch_dir receives each compile's intermediate files and must exist.
    [[nodiscard]] static core::Result<ShaderCompiler, std::string> create(const std::string& dxc_path, const std::string& scratch_dir);

    // SHA-256 (hex) of the compiler executable and the compiler libraries beside it. A new toolchain changes it.
    [[nodiscard]] const std::string& fingerprint() const noexcept {
        return fingerprint_;
    }

    [[nodiscard]] CompileResult compile(const CompileRequest& request) const;

private:
    ShaderCompiler(std::string dxc_path, std::string scratch_dir, std::string fingerprint);

    std::string dxc_path_;
    std::string scratch_dir_;
    std::string fingerprint_;
};

}  // namespace engine::shader
