#pragma once

// Compiled-pipeline cache (US-031). An entry holds the bytecode and reflection of one compile, keyed by its request and by the
// environment it was built in. Its dependencies (the source and every include) are stamped with their content hashes, so an
// entry is reused only while every file it read is unchanged. The cache file records its environment, and a file written for a
// different compiler, backend, or adapter is rejected instead of being merged.

#include "engine/assets/content_hash.hpp"
#include "engine/core/result.hpp"
#include "engine/rhi/capabilities.hpp"
#include "engine/shader/compiler.hpp"
#include "engine/shader/reflection.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace engine::shader {

// Everything outside the request that can change the output: the compiler fingerprint, the backend, and the adapter.
[[nodiscard]] std::string make_environment(const ShaderCompiler& compiler, const rhi::CapabilityReport& report);

// Hex SHA-256 over the request (source path, entry, profile, target, include directories, and defines in name order) and the
// environment. The source contents and the include contents are not part of it; they are dependency stamps (see CacheEntry).
[[nodiscard]] std::string request_key(const CompileRequest& request, const std::string& environment);

// SHA-256 of a file's current contents.
[[nodiscard]] core::Result<assets::ContentHash, std::string> hash_file(const std::string& path);

struct CacheDependency {
    std::string path;
    assets::ContentHash hash;  // contents when the entry was compiled
};

struct CacheEntry {
    std::string request_key;
    std::vector<CacheDependency> dependencies;  // the source first, then the includes
    std::vector<std::uint8_t> bytecode;
    ShaderReflection reflection;
};

class PipelineCache {
public:
    explicit PipelineCache(std::string environment);

    [[nodiscard]] const std::string& environment() const noexcept {
        return environment_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

    // The entry for `key`, or nullptr when none exists or one of its dependencies no longer matches the file on disk. A changed
    // dependency is a miss, and the stale entry stays until insert() replaces it.
    [[nodiscard]] const CacheEntry* find(const std::string& key) const;

    // Stores the entry, replacing any entry with the same request key.
    void insert(CacheEntry entry);

    // Writes the cache atomically. The parent directory must exist.
    [[nodiscard]] core::Result<core::Unit, std::string> save(const std::string& path) const;

    // Reads a cache file for `environment`. A missing file gives an empty cache. A file with another environment, a bad magic or
    // version, or a truncated body is an error, and nothing is loaded from it.
    [[nodiscard]] static core::Result<PipelineCache, std::string> load(const std::string& path, std::string environment);

private:
    std::string environment_;
    std::map<std::string, CacheEntry> entries_;
};

}  // namespace engine::shader
