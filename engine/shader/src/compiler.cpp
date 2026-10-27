#include "engine/shader/compiler.hpp"

#include "engine/assets/content_hash.hpp"
#include "engine/core/file_io.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace engine::shader {
namespace {

namespace fs = std::filesystem;

core::Result<std::string, std::string> read_bytes(const std::string& path) {
    auto parsed = core::Path::from_utf8(path);
    if (!parsed.ok()) {
        return core::failure("invalid path: " + path);
    }
    auto bytes = core::read_file(parsed.value());
    if (!bytes.ok()) {
        return core::failure(core::describe(bytes.error()));
    }
    return core::success(std::move(bytes).value());
}

// Quotes one argument for the platform shell that std::system uses.
std::string quote(const std::string& arg) {
#ifdef _WIN32
    std::string out = "\"";
    for (const char c : arg) {
        if (c == '"') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
#else
    std::string out = "'";
    for (const char c : arg) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    return out + "'";
#endif
}

// Runs the tool. Its stdout and stderr go to the given files; an empty stderr_path merges stderr into stdout's file.
// Returns the exit status, or -1 when the tool did not run.
int run_tool(const std::vector<std::string>& argv, const std::string& stdout_path, const std::string& stderr_path) {
    std::string line;
    for (const std::string& arg : argv) {
        if (!line.empty()) {
            line += ' ';
        }
        line += quote(arg);
    }
    line += " > " + quote(stdout_path);
    line += stderr_path.empty() ? std::string(" 2>&1") : " 2> " + quote(stderr_path);
#ifdef _WIN32
    // cmd.exe /c drops one pair of outer quotes, so the whole line gets an extra pair.
    line = "\"" + line + "\"";
    return std::system(line.c_str());
#else
    const int status = std::system(line.c_str());
    if (status == -1 || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
#endif
}

// Make-style dependency file (dxc -M): "<target>: <dep> <dep> \" with continuation lines and escaped spaces. Windows paths
// use backslashes, so only the escapes above are decoded.
std::vector<std::string> parse_make_dependencies(const std::string& text) {
    std::string joined;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r')) {
            ++i;
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            joined += ' ';
            continue;
        }
        joined += text[i];
    }

    std::vector<std::string> tokens;
    std::string current;
    bool in_token = false;
    for (std::size_t i = 0; i < joined.size(); ++i) {
        const char c = joined[i];
        // Make escapes only a space, a tab, or '#'. Any other backslash is a Windows path separator, which must survive.
        const bool escapes = c == '\\' && i + 1 < joined.size() &&
                             (joined[i + 1] == ' ' || joined[i + 1] == '\t' || joined[i + 1] == '#');
        if (escapes) {
            current += joined[++i];
            in_token = true;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (in_token) {
                tokens.push_back(current);
                current.clear();
                in_token = false;
            }
        } else {
            current += c;
            in_token = true;
        }
    }
    if (in_token) {
        tokens.push_back(current);
    }

    // The first token is the target, and the rest are the files the compile read.
    if (!tokens.empty()) {
        tokens.erase(tokens.begin());
    }
    return tokens;
}

core::Result<std::string, std::string> fingerprint_toolchain(const fs::path& dxc) {
    std::error_code ec;
    if (!fs::is_regular_file(dxc, ec)) {
        return core::failure("shader compiler not found: " + dxc.string());
    }

    // The executable plus the DXC libraries beside it (bin/ on Windows, bin/ and lib/ on Linux).
    std::vector<std::string> files{dxc.string()};
    for (const fs::path& dir : {dxc.parent_path(), dxc.parent_path().parent_path() / "lib"}) {
        if (!fs::is_directory(dir, ec)) {
            continue;
        }
        for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
            const std::string name = entry.path().filename().string();
            if (entry.is_regular_file(ec) && (name.find("dxcompiler") != std::string::npos || name.find("dxil") != std::string::npos)) {
                files.push_back(entry.path().string());
            }
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    std::string material;
    for (const std::string& file : files) {
        auto bytes = read_bytes(file);
        if (!bytes.ok()) {
            return core::failure(bytes.error());
        }
        material += fs::path(file).filename().string() + ":" + assets::ContentHash::of(bytes.value()).to_hex() + "\n";
    }
    return core::success(assets::ContentHash::of(material).to_hex());
}

}  // namespace

core::Result<ShaderCompiler, std::string> ShaderCompiler::create(const std::string& dxc_path, const std::string& scratch_dir) {
    std::error_code ec;
    if (!fs::is_directory(scratch_dir, ec)) {
        return core::failure("shader scratch directory not found: " + scratch_dir);
    }
    auto fingerprint = fingerprint_toolchain(fs::path(dxc_path));
    if (!fingerprint.ok()) {
        return core::failure(fingerprint.error());
    }
    return core::success(ShaderCompiler(dxc_path, scratch_dir, std::move(fingerprint).value()));
}

ShaderCompiler::ShaderCompiler(std::string dxc_path, std::string scratch_dir, std::string fingerprint)
    : dxc_path_(std::move(dxc_path)), scratch_dir_(std::move(scratch_dir)), fingerprint_(std::move(fingerprint)) {}

CompileResult ShaderCompiler::compile(const CompileRequest& request) const {
    CompileResult result;
    const auto fail = [&result](std::string message) {
        Diagnostic diagnostic;
        diagnostic.severity = Severity::Error;
        diagnostic.file = std::string();
        diagnostic.message = std::move(message);
        result.diagnostics.push_back(std::move(diagnostic));
        return result;
    };

    std::error_code ec;
    const fs::path source = fs::absolute(request.source_path, ec);
    if (ec || !fs::is_regular_file(source, ec)) {
        return fail("source file not found: " + request.source_path);
    }

    // Intermediate names come from the request, so two compiles of the same request reuse one set of files.
    std::string signature = source.string() + "|" + request.entry_point + "|" + request.profile + "|" +
                            std::string(to_string(request.target));
    for (const std::string& dir : request.include_dirs) {
        signature += "|I" + dir;
    }
    std::vector<ShaderDefine> defines = request.defines;
    std::sort(defines.begin(), defines.end(), [](const ShaderDefine& a, const ShaderDefine& b) { return a.name < b.name; });
    for (const ShaderDefine& define : defines) {
        signature += "|D" + define.name + "=" + define.value;
    }
    const std::string stem = (fs::path(scratch_dir_) / assets::ContentHash::of(signature).to_hex().substr(0, 16)).string();
    const std::string out_bin = stem + ".bin";
    const std::string listing = stem + ".lst";
    const std::string deps = stem + ".dep";
    const std::string log = stem + ".log";
    const std::string dep_log = stem + ".dlog";
    for (const std::string& path : {out_bin, listing, deps, log, dep_log}) {
        fs::remove(path, ec);
    }

    // Arguments shared by the compile and the dependency pass.
    std::vector<std::string> shared = {"-nologo", "-T", request.profile, "-E", request.entry_point};
    if (request.target == ShaderTarget::SpirV) {
        // The DX layout keeps CPU and GPU struct offsets identical on both targets (see validate_cpu_layouts).
        // SPV_EXT_mesh_shader lets a mesh or task entry (US-038) compile to SPIR-V; other entries do not use it.
        shared.insert(shared.end(), {"-spirv", "-fspv-target-env=vulkan1.2", "-fspv-extension=SPV_EXT_mesh_shader", "-fvk-use-dx-layout"});
    }
    for (const std::string& dir : request.include_dirs) {
        shared.push_back("-I");
        shared.push_back(dir);
    }
    for (const ShaderDefine& define : defines) {
        shared.push_back("-D" + define.name + "=" + define.value);
    }

    // A dependency flag (-MF, -MD) makes dxc stop after preprocessing and write no output, so the include list comes from a
    // second, preprocess-only pass (-M) that runs after a successful compile.
    std::vector<std::string> argv = {dxc_path_};
    argv.insert(argv.end(), shared.begin(), shared.end());
    argv.insert(argv.end(), {"-Fo", out_bin, "-Fc", listing});
    argv.push_back(source.string());

    const int status = run_tool(argv, log, std::string());
    const auto log_text = read_bytes(log);
    result.diagnostics = parse_compiler_output(log_text.ok() ? log_text.value() : std::string());
    if (status != 0 && !has_errors(result.diagnostics)) {
        result.diagnostics.push_back(Diagnostic{Severity::Error, std::string(), 0, 0,
                                                status < 0 ? "could not run the shader compiler: " + dxc_path_
                                                           : "the shader compiler failed with status " + std::to_string(status)});
    }
    if (status != 0 || has_errors(result.diagnostics)) {
        return result;
    }

    auto bytes = read_bytes(out_bin);
    if (!bytes.ok()) {
        return fail("the shader compiler wrote no output: " + bytes.error());
    }
    result.bytecode.assign(bytes.value().begin(), bytes.value().end());

    auto listing_text = read_bytes(listing);
    if (!listing_text.ok()) {
        return fail("the shader compiler wrote no listing: " + listing_text.error());
    }
    auto reflected = request.target == ShaderTarget::Dxil ? parse_dxil_listing(listing_text.value())
                                                          : parse_spirv_listing(listing_text.value());
    if (!reflected.ok()) {
        result.bytecode.clear();
        return fail("reflection failed: " + reflected.error());
    }
    result.reflection = std::move(reflected).value();

    result.dependencies.push_back(source.string());
    std::vector<std::string> dep_argv = {dxc_path_};
    dep_argv.insert(dep_argv.end(), shared.begin(), shared.end());
    dep_argv.insert(dep_argv.end(), {"-M", source.string()});
    const int dep_status = run_tool(dep_argv, deps, dep_log);
    const auto dep_text = read_bytes(deps);
    if (dep_status != 0 || !dep_text.ok()) {
        // The cache can still stamp the source; an include it missed would only make a stale entry look current.
        result.diagnostics.push_back(Diagnostic{Severity::Warning, std::string(), 0, 0,
                                                "could not list the includes of " + source.string() + "; only the source is stamped"});
    } else {
        for (std::string& dependency : parse_make_dependencies(dep_text.value())) {
            if (std::find(result.dependencies.begin(), result.dependencies.end(), dependency) == result.dependencies.end()) {
                result.dependencies.push_back(std::move(dependency));
            }
        }
    }
    result.ok = true;
    return result;
}

}  // namespace engine::shader
