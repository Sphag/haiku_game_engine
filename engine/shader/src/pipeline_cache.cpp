#include "engine/shader/pipeline_cache.hpp"

#include "engine/core/file_io.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <utility>

namespace engine::shader {
namespace {

constexpr char kMagic[4] = {'O', 'W', 'P', 'C'};
constexpr std::uint32_t kFormatVersion = 1;

// Little-endian binary writer. Every length is a u32, which is enough for any shader and any cache this engine writes.
class Writer {
public:
    void u8(std::uint8_t value) {
        out_.push_back(static_cast<char>(value));
    }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out_.push_back(static_cast<char>((value >> shift) & 0xFFu));
        }
    }
    void raw(const void* data, std::size_t size) {
        out_.append(static_cast<const char*>(data), size);
    }
    void str(std::string_view text) {
        u32(static_cast<std::uint32_t>(text.size()));
        out_.append(text.data(), text.size());
    }
    void bytes(const std::vector<std::uint8_t>& data) {
        u32(static_cast<std::uint32_t>(data.size()));
        raw(data.data(), data.size());
    }
    [[nodiscard]] const std::string& text() const noexcept {
        return out_;
    }

private:
    std::string out_;
};

// Bounds-checked reader. Every read reports false instead of running past the end of the file.
class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    bool u8(std::uint8_t& value) {
        if (remaining() < 1) {
            return false;
        }
        value = static_cast<std::uint8_t>(data_[pos_++]);
        return true;
    }
    bool u32(std::uint32_t& value) {
        if (remaining() < 4) {
            return false;
        }
        value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[pos_++])) << shift;
        }
        return true;
    }
    bool raw(void* destination, std::size_t size) {
        if (remaining() < size) {
            return false;
        }
        if (size != 0) {
            std::memcpy(destination, data_.data() + pos_, size);
        }
        pos_ += size;
        return true;
    }
    bool str(std::string& text) {
        std::uint32_t size = 0;
        if (!u32(size) || remaining() < size) {
            return false;
        }
        text.assign(data_.substr(pos_, size));
        pos_ += size;
        return true;
    }
    bool bytes(std::vector<std::uint8_t>& data) {
        std::uint32_t size = 0;
        if (!u32(size) || remaining() < size) {
            return false;
        }
        data.resize(size);
        if (size != 0) {
            std::memcpy(data.data(), data_.data() + pos_, size);
        }
        pos_ += size;
        return true;
    }

private:
    [[nodiscard]] std::size_t remaining() const noexcept {
        return data_.size() - pos_;
    }

    std::string_view data_;
    std::size_t pos_ = 0;
};

void write_reflection(Writer& out, const ShaderReflection& reflection) {
    out.u8(static_cast<std::uint8_t>(reflection.target));
    out.u32(static_cast<std::uint32_t>(reflection.bindings.size()));
    for (const ReflectedBinding& binding : reflection.bindings) {
        out.str(binding.name);
        out.u8(static_cast<std::uint8_t>(binding.cls));
        out.u32(binding.set);
        out.u32(binding.binding);
        out.u32(binding.count);
    }
    out.u32(static_cast<std::uint32_t>(reflection.blocks.size()));
    for (const ReflectedBlock& block : reflection.blocks) {
        out.str(block.name);
        out.u32(block.set);
        out.u32(block.binding);
        out.u32(block.size_bytes);
        out.u32(static_cast<std::uint32_t>(block.members.size()));
        for (const ReflectedMember& member : block.members) {
            out.str(member.name);
            out.u32(member.offset);
        }
    }
}

bool read_reflection(Reader& in, ShaderReflection& reflection) {
    std::uint8_t target = 0;
    if (!in.u8(target) || target > static_cast<std::uint8_t>(ShaderTarget::SpirV)) {
        return false;
    }
    reflection.target = static_cast<ShaderTarget>(target);

    std::uint32_t count = 0;
    if (!in.u32(count)) {
        return false;
    }
    reflection.bindings.resize(count);
    for (ReflectedBinding& binding : reflection.bindings) {
        std::uint8_t cls = 0;
        if (!in.str(binding.name) || !in.u8(cls) || cls > static_cast<std::uint8_t>(BindingClass::Sampler) || !in.u32(binding.set) ||
            !in.u32(binding.binding) || !in.u32(binding.count)) {
            return false;
        }
        binding.cls = static_cast<BindingClass>(cls);
    }

    if (!in.u32(count)) {
        return false;
    }
    reflection.blocks.resize(count);
    for (ReflectedBlock& block : reflection.blocks) {
        std::uint32_t members = 0;
        if (!in.str(block.name) || !in.u32(block.set) || !in.u32(block.binding) || !in.u32(block.size_bytes) || !in.u32(members)) {
            return false;
        }
        block.members.resize(members);
        for (ReflectedMember& member : block.members) {
            if (!in.str(member.name) || !in.u32(member.offset)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

std::string make_environment(const ShaderCompiler& compiler, const rhi::CapabilityReport& report) {
    return "format=1;compiler=" + compiler.fingerprint() + ";backend=" + std::string(rhi::to_string(report.backend)) +
           ";adapter=" + report.adapter_name;
}

std::string request_key(const CompileRequest& request, const std::string& environment) {
    std::string material = "request-key-v1\n";
    material += "environment=" + environment + "\n";
    material += "source=" + request.source_path + "\n";
    material += "entry=" + request.entry_point + "\n";
    material += "profile=" + request.profile + "\n";
    material += "target=" + std::string(to_string(request.target)) + "\n";
    for (const std::string& dir : request.include_dirs) {
        material += "include=" + dir + "\n";
    }
    std::vector<ShaderDefine> defines = request.defines;
    std::sort(defines.begin(), defines.end(), [](const ShaderDefine& a, const ShaderDefine& b) { return a.name < b.name; });
    for (const ShaderDefine& define : defines) {
        material += "define=" + define.name + "=" + define.value + "\n";
    }
    return assets::ContentHash::of(material).to_hex();
}

core::Result<assets::ContentHash, std::string> hash_file(const std::string& path) {
    auto parsed = core::Path::from_utf8(path);
    if (!parsed.ok()) {
        return core::failure("invalid path: " + path);
    }
    auto bytes = core::read_file(parsed.value());
    if (!bytes.ok()) {
        return core::failure(core::describe(bytes.error()));
    }
    return core::success(assets::ContentHash::of(bytes.value()));
}

PipelineCache::PipelineCache(std::string environment) : environment_(std::move(environment)) {}

const CacheEntry* PipelineCache::find(const std::string& key) const {
    const auto entry = entries_.find(key);
    if (entry == entries_.end()) {
        return nullptr;
    }
    for (const CacheDependency& dependency : entry->second.dependencies) {
        const auto current = hash_file(dependency.path);
        if (!current.ok() || current.value() != dependency.hash) {
            return nullptr;
        }
    }
    return &entry->second;
}

void PipelineCache::insert(CacheEntry entry) {
    const std::string key = entry.request_key;
    entries_[key] = std::move(entry);
}

core::Result<core::Unit, std::string> PipelineCache::save(const std::string& path) const {
    Writer out;
    out.raw(kMagic, sizeof kMagic);
    out.u32(kFormatVersion);
    out.str(environment_);
    out.u32(static_cast<std::uint32_t>(entries_.size()));
    for (const auto& [key, entry] : entries_) {
        out.str(key);
        out.u32(static_cast<std::uint32_t>(entry.dependencies.size()));
        for (const CacheDependency& dependency : entry.dependencies) {
            out.str(dependency.path);
            out.raw(dependency.hash.bytes().data(), dependency.hash.bytes().size());
        }
        out.bytes(entry.bytecode);
        write_reflection(out, entry.reflection);
    }

    auto parsed = core::Path::from_utf8(path);
    if (!parsed.ok()) {
        return core::failure("invalid path: " + path);
    }
    auto written = core::write_file_atomic(parsed.value(), out.text());
    if (!written.ok()) {
        return core::failure(core::describe(written.error()));
    }
    return core::success();
}

core::Result<PipelineCache, std::string> PipelineCache::load(const std::string& path, std::string environment) {
    auto parsed = core::Path::from_utf8(path);
    if (!parsed.ok()) {
        return core::failure("invalid path: " + path);
    }
    auto file = core::read_file(parsed.value());
    if (!file.ok()) {
        if (file.error().code == core::IoErrorCode::NotFound) {
            return core::success(PipelineCache(std::move(environment)));
        }
        return core::failure(core::describe(file.error()));
    }

    Reader in(file.value());
    char magic[4] = {};
    std::uint32_t version = 0;
    if (!in.raw(magic, sizeof magic) || !std::equal(magic, magic + 4, kMagic)) {
        return core::failure("not a pipeline cache: " + path);
    }
    if (!in.u32(version) || version != kFormatVersion) {
        return core::failure("unsupported pipeline cache version in " + path);
    }
    std::string found;
    if (!in.str(found)) {
        return core::failure("pipeline cache is truncated: " + path);
    }
    if (found != environment) {
        return core::failure("pipeline cache " + path + " was built for [" + found + "] but the current environment is [" +
                             environment + "]; delete it to rebuild");
    }

    PipelineCache cache(std::move(environment));
    std::uint32_t count = 0;
    if (!in.u32(count)) {
        return core::failure("pipeline cache is truncated: " + path);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        CacheEntry entry;
        std::uint32_t dependency_count = 0;
        if (!in.str(entry.request_key) || !in.u32(dependency_count)) {
            return core::failure("pipeline cache is truncated: " + path);
        }
        for (std::uint32_t d = 0; d < dependency_count; ++d) {
            CacheDependency dependency;
            std::array<std::uint8_t, 32> digest{};
            if (!in.str(dependency.path) || !in.raw(digest.data(), digest.size())) {
                return core::failure("pipeline cache is truncated: " + path);
            }
            dependency.hash = assets::ContentHash(digest);
            entry.dependencies.push_back(std::move(dependency));
        }
        if (!in.bytes(entry.bytecode) || !read_reflection(in, entry.reflection)) {
            return core::failure("pipeline cache is truncated or corrupt: " + path);
        }
        cache.entries_[entry.request_key] = std::move(entry);
    }
    return core::success(std::move(cache));
}

}  // namespace engine::shader
