// Compiled-pipeline cache (US-031): keys, dependency stamps, and the cache file. The file tests write into the build tree.

#include "engine/shader/pipeline_cache.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using engine::shader::CacheDependency;
using engine::shader::CacheEntry;
using engine::shader::CompileRequest;
using engine::shader::hash_file;
using engine::shader::PipelineCache;
using engine::shader::request_key;
using engine::shader::ShaderDefine;
using engine::shader::ShaderTarget;

namespace fs = std::filesystem;

namespace {

// A fresh directory under the build tree for one test. Each test name gets its own directory.
fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(ENGINE_TEST_SHADER_SCRATCH) / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

CompileRequest base_request() {
    CompileRequest request;
    request.source_path = "shaders/compute.hlsl";
    request.entry_point = "main";
    request.profile = "cs_6_0";
    request.target = ShaderTarget::Dxil;
    request.include_dirs = {"shaders/common"};
    request.defines = {ShaderDefine{"QUALITY", "2"}, ShaderDefine{"DEBUG", ""}};
    return request;
}

CacheEntry make_entry(const std::string& key, const fs::path& source) {
    CacheEntry entry;
    entry.request_key = key;
    entry.dependencies.push_back(CacheDependency{source.string(), hash_file(source.string()).value()});
    entry.bytecode = {0x44, 0x58, 0x42, 0x43, 1, 2, 3};
    entry.reflection.target = ShaderTarget::Dxil;
    engine::shader::ReflectedBinding binding;
    binding.name = "Output";
    binding.cls = engine::shader::BindingClass::UnorderedAccess;
    binding.set = 0;
    binding.binding = 2;
    binding.count = 1;
    entry.reflection.bindings.push_back(binding);
    engine::shader::ReflectedBlock block;
    block.name = "Params";
    block.size_bytes = 16;
    block.members = {engine::shader::ReflectedMember{"Scale", 0}};
    entry.reflection.blocks.push_back(block);
    return entry;
}

}  // namespace

TEST_CASE("A request key changes with every input that changes the output and not with define order", "[unit]") {
    const std::string env = "format=1;compiler=abc;backend=dx12;adapter=test";
    const std::string base = request_key(base_request(), env);
    CHECK(base.size() == 64);
    CHECK(request_key(base_request(), env) == base);

    CompileRequest reordered = base_request();
    reordered.defines = {ShaderDefine{"DEBUG", ""}, ShaderDefine{"QUALITY", "2"}};
    CHECK(request_key(reordered, env) == base);

    CompileRequest changed_define = base_request();
    changed_define.defines[0].value = "3";
    CHECK(request_key(changed_define, env) != base);

    CompileRequest changed_entry = base_request();
    changed_entry.entry_point = "CSMain";
    CHECK(request_key(changed_entry, env) != base);

    CompileRequest changed_profile = base_request();
    changed_profile.profile = "cs_6_6";
    CHECK(request_key(changed_profile, env) != base);

    CompileRequest changed_target = base_request();
    changed_target.target = ShaderTarget::SpirV;
    CHECK(request_key(changed_target, env) != base);

    CompileRequest changed_include = base_request();
    changed_include.include_dirs = {"shaders/other"};
    CHECK(request_key(changed_include, env) != base);

    // The environment covers the compiler, the backend, and the adapter.
    CHECK(request_key(base_request(), "format=1;compiler=xyz;backend=dx12;adapter=test") != base);
    CHECK(request_key(base_request(), "format=1;compiler=abc;backend=vulkan;adapter=test") != base);
    CHECK(request_key(base_request(), "format=1;compiler=abc;backend=dx12;adapter=other") != base);
}

TEST_CASE("A cache entry survives a save and load with its bytecode and reflection intact", "[unit]") {
    const fs::path dir = scratch("cache_round_trip");
    const fs::path source = dir / "compute.hlsl";
    write_text(source, "float x;");
    const std::string env = "format=1;compiler=abc;backend=validation;adapter=cpu";
    const std::string key = request_key(base_request(), env);

    PipelineCache cache(env);
    cache.insert(make_entry(key, source));
    REQUIRE(cache.save((dir / "cache.bin").string()).ok());

    auto loaded = PipelineCache::load((dir / "cache.bin").string(), env);
    REQUIRE(loaded.ok());
    PipelineCache restored = std::move(loaded).value();
    REQUIRE(restored.size() == 1);

    const CacheEntry* entry = restored.find(key);
    REQUIRE(entry != nullptr);
    CHECK(entry->bytecode == std::vector<std::uint8_t>({0x44, 0x58, 0x42, 0x43, 1, 2, 3}));
    REQUIRE(entry->reflection.bindings.size() == 1);
    CHECK(entry->reflection.bindings[0].name == "Output");
    CHECK(entry->reflection.bindings[0].cls == engine::shader::BindingClass::UnorderedAccess);
    REQUIRE(entry->reflection.blocks.size() == 1);
    CHECK(entry->reflection.blocks[0].size_bytes == 16);
    CHECK(entry->reflection.blocks[0].members[0].name == "Scale");
}

TEST_CASE("A cache entry is a miss once any file it read has changed", "[unit]") {
    const fs::path dir = scratch("cache_stale");
    const fs::path source = dir / "compute.hlsl";
    write_text(source, "float x;");
    const std::string env = "format=1;compiler=abc;backend=validation;adapter=cpu";
    const std::string key = request_key(base_request(), env);

    PipelineCache cache(env);
    cache.insert(make_entry(key, source));
    REQUIRE(cache.find(key) != nullptr);

    write_text(source, "float y;");
    CHECK(cache.find(key) == nullptr);

    // Recompiling stores the new stamp, and the entry is valid again.
    cache.insert(make_entry(key, source));
    CHECK(cache.find(key) != nullptr);
    CHECK(cache.size() == 1);
}

TEST_CASE("A cache written for another environment is rejected, never merged", "[unit]") {
    const fs::path dir = scratch("cache_environment");
    const fs::path source = dir / "compute.hlsl";
    write_text(source, "float x;");
    const std::string written = "format=1;compiler=old;backend=dx12;adapter=RTX 5080";
    PipelineCache cache(written);
    cache.insert(make_entry(request_key(base_request(), written), source));
    REQUIRE(cache.save((dir / "cache.bin").string()).ok());

    const std::string current = "format=1;compiler=new;backend=dx12;adapter=RTX 5080";
    auto rejected = PipelineCache::load((dir / "cache.bin").string(), current);
    REQUIRE_FALSE(rejected.ok());
    CHECK(rejected.error().find("was built for [" + written + "]") != std::string::npos);
    CHECK(rejected.error().find("delete it to rebuild") != std::string::npos);
}

TEST_CASE("A damaged cache file is rejected, and a missing one is an empty cache", "[unit]") {
    const fs::path dir = scratch("cache_damaged");
    const fs::path source = dir / "compute.hlsl";
    write_text(source, "float x;");
    const std::string env = "format=1;compiler=abc;backend=validation;adapter=cpu";
    PipelineCache cache(env);
    cache.insert(make_entry(request_key(base_request(), env), source));
    REQUIRE(cache.save((dir / "cache.bin").string()).ok());

    std::stringstream buffer;
    {
        std::ifstream in(dir / "cache.bin", std::ios::binary);
        buffer << in.rdbuf();
    }
    const std::string full = buffer.str();

    write_text(dir / "truncated.bin", full.substr(0, full.size() - 5));
    auto truncated = PipelineCache::load((dir / "truncated.bin").string(), env);
    REQUIRE_FALSE(truncated.ok());
    CHECK(truncated.error().find("truncated") != std::string::npos);

    write_text(dir / "magic.bin", "NOPE" + full.substr(4));
    auto bad_magic = PipelineCache::load((dir / "magic.bin").string(), env);
    REQUIRE_FALSE(bad_magic.ok());
    CHECK(bad_magic.error().find("not a pipeline cache") != std::string::npos);

    auto missing = PipelineCache::load((dir / "absent.bin").string(), env);
    REQUIRE(missing.ok());
    CHECK(missing.value().size() == 0);
}
