#include "engine/shader/reflection.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace engine::shader {
namespace {

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        begin = end + 1;
    }
    return lines;
}

std::vector<std::string_view> split_fields(std::string_view text) {
    std::vector<std::string_view> fields;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
            ++i;
        }
        const std::size_t begin = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') {
            ++i;
        }
        if (i > begin) {
            fields.push_back(text.substr(begin, i - begin));
        }
    }
    return fields;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == ';')) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<std::uint32_t> parse_u32(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
        if (value > 0xFFFFFFFFull) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

// Text after a label such as "; Offset:" up to the first space-separated number.
std::optional<std::uint32_t> number_after(std::string_view line, std::string_view label) {
    const std::size_t at = line.find(label);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const auto fields = split_fields(line.substr(at + label.size()));
    if (fields.empty()) {
        return std::nullopt;
    }
    return parse_u32(fields.front());
}

std::string describe_binding(const ReflectedBinding& binding) {
    std::string text = binding.name.empty() ? std::string("binding") : binding.name;
    text += " (set ";
    text += std::to_string(binding.set);
    text += ", binding ";
    text += std::to_string(binding.binding);
    text += ")";
    return text;
}

Diagnostic error_at(const std::string& source, std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.file = source;
    diagnostic.message = std::move(message);
    return diagnostic;
}

void sort_reflection(ShaderReflection& reflection) {
    std::sort(reflection.bindings.begin(), reflection.bindings.end(),
              [](const ReflectedBinding& a, const ReflectedBinding& b) {
                  return a.set != b.set ? a.set < b.set : a.binding < b.binding;
              });
    std::sort(reflection.blocks.begin(), reflection.blocks.end(), [](const ReflectedBlock& a, const ReflectedBlock& b) {
        return a.set != b.set ? a.set < b.set : a.binding < b.binding;
    });
}

// ---- DXIL ---------------------------------------------------------------------------------------------------------------

// The "Buffer Definitions" section lists each cbuffer's members with "; Offset: N" and closes it with "Size: N".
std::map<std::string, ReflectedBlock> dxil_buffer_definitions(const std::vector<std::string_view>& lines) {
    std::map<std::string, ReflectedBlock> blocks;
    std::string current;
    for (const std::string_view line : lines) {
        if (line.rfind("; cbuffer ", 0) == 0) {
            current = std::string(trim(line.substr(10)));
            blocks[current].name = current;
            continue;
        }
        if (current.empty()) {
            continue;
        }
        const std::size_t offset_at = line.find("; Offset:");
        if (offset_at == std::string_view::npos || offset_at < 2) {
            continue;
        }
        ReflectedBlock& block = blocks[current];
        if (line.find("Size:") != std::string_view::npos) {
            // The closing line "} Name; ; Offset: 0 Size: 16" ends the block.
            block.size_bytes = number_after(line, "Size:").value_or(0);
            current.clear();
            continue;
        }
        // A member line reads "; <type> <name>[<count>];   ; Offset: N". The name is the last word before the semicolon.
        std::string_view code = line.substr(2, offset_at - 2);
        code = trim(code);
        const std::size_t bracket = code.find('[');
        if (bracket != std::string_view::npos) {
            code = code.substr(0, bracket);
        }
        const auto fields = split_fields(code);
        if (fields.empty()) {
            continue;
        }
        ReflectedMember member;
        member.name = std::string(fields.back());
        member.offset = number_after(line, "; Offset:").value_or(0);
        block.members.push_back(std::move(member));
    }
    return blocks;
}

core::Result<BindingClass, std::string> dxil_class(std::string_view type) {
    if (type == "cbuffer") {
        return core::success(BindingClass::ConstantBuffer);
    }
    if (type == "texture" || type == "struct") {
        return core::success(BindingClass::ShaderResource);
    }
    if (type == "UAV") {
        return core::success(BindingClass::UnorderedAccess);
    }
    if (type == "sampler") {
        return core::success(BindingClass::Sampler);
    }
    return core::failure("unsupported DXIL resource type '" + std::string(type) + "'");
}

}  // namespace

core::Result<ShaderReflection, std::string> parse_dxil_listing(std::string_view listing) {
    const auto lines = split_lines(listing);
    auto definitions = dxil_buffer_definitions(lines);

    ShaderReflection reflection;
    reflection.target = ShaderTarget::Dxil;
    // The table is "; Resource Bindings:", a blank comment, the "; Name" header, a dashed rule, then one row per resource. A blank
    // comment ends the table only after the header, so the blank line before the header is skipped.
    bool in_table = false;
    bool header_seen = false;
    for (const std::string_view line : lines) {
        if (line.rfind("; Resource Bindings:", 0) == 0) {
            in_table = true;
            continue;
        }
        if (!in_table) {
            continue;
        }
        if (!header_seen) {
            if (line.rfind("; Name", 0) == 0) {
                header_seen = true;
            }
            continue;
        }
        if (line.rfind("; ---", 0) == 0) {
            continue;
        }
        if (line.rfind("; ", 0) != 0) {
            break;
        }
        const auto fields = split_fields(line.substr(2));
        if (fields.size() < 7) {
            return core::failure("malformed resource binding row: " + std::string(line));
        }
        auto cls = dxil_class(fields[1]);
        if (!cls.ok()) {
            return core::failure(cls.error());
        }

        // The bind column reads "t4" or "cb3,space1". The register follows the letters, and the space defaults to 0.
        std::string_view bind = fields[fields.size() - 2];
        std::uint32_t space = 0;
        const std::size_t comma = bind.find(',');
        if (comma != std::string_view::npos) {
            const std::string_view space_text = bind.substr(comma + 1);
            const auto parsed = space_text.rfind("space", 0) == 0 ? parse_u32(space_text.substr(5)) : std::nullopt;
            if (!parsed) {
                return core::failure("malformed register space in binding row: " + std::string(line));
            }
            space = *parsed;
            bind = bind.substr(0, comma);
        }
        const std::size_t digits = bind.find_first_of("0123456789");
        const auto reg = digits == std::string_view::npos ? std::nullopt : parse_u32(bind.substr(digits));
        const auto count = parse_u32(fields.back());
        if (!reg || !count) {
            return core::failure("malformed register or count in binding row: " + std::string(line));
        }

        ReflectedBinding binding;
        binding.name = std::string(fields[0]);
        binding.cls = cls.value();
        binding.set = space;
        binding.binding = *reg;
        binding.count = *count;
        reflection.bindings.push_back(binding);

        if (binding.cls == BindingClass::ConstantBuffer) {
            ReflectedBlock block;
            const auto found = definitions.find(binding.name);
            if (found != definitions.end()) {
                block = found->second;
            }
            block.name = binding.name;
            block.set = space;
            block.binding = *reg;
            reflection.blocks.push_back(std::move(block));
        }
    }
    sort_reflection(reflection);
    return core::success(std::move(reflection));
}

// ---- SPIR-V -------------------------------------------------------------------------------------------------------------

namespace {

struct SpvType {
    std::string op;
    std::vector<std::string> operands;
};

struct SpvVariable {
    std::string type;  // the OpTypePointer id
    std::string storage;
};

struct SpvDecorations {
    std::optional<std::uint32_t> set;
    std::optional<std::uint32_t> binding;
    bool non_writable = false;
};

std::string unquote(std::string_view text) {
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    return std::string(text);
}

}  // namespace

core::Result<ShaderReflection, std::string> parse_spirv_listing(std::string_view listing) {
    std::map<std::string, std::string> names;
    std::map<std::string, std::map<std::uint32_t, std::string>> member_names;
    std::map<std::string, std::map<std::uint32_t, std::uint32_t>> member_offsets;
    std::map<std::string, std::set<std::uint32_t>> member_non_writable;
    std::map<std::string, SpvDecorations> decorations;
    std::map<std::string, SpvType> types;
    std::map<std::string, std::uint32_t> constants;
    std::map<std::string, SpvVariable> variables;

    for (const std::string_view line : split_lines(listing)) {
        auto fields = split_fields(line);
        if (fields.empty() || fields[0].front() == ';') {
            continue;
        }
        std::string result;
        if (fields.size() >= 3 && fields[0].front() == '%' && fields[1] == "=") {
            result = std::string(fields[0]);
            fields.erase(fields.begin(), fields.begin() + 2);
        }
        if (fields.empty()) {
            continue;
        }
        const std::string_view op = fields[0];
        const std::vector<std::string_view> args(fields.begin() + 1, fields.end());

        if (op == "OpName" && args.size() >= 2) {
            names[std::string(args[0])] = unquote(args[1]);
        } else if (op == "OpMemberName" && args.size() >= 3) {
            const auto index = parse_u32(args[1]);
            if (index) {
                member_names[std::string(args[0])][*index] = unquote(args[2]);
            }
        } else if (op == "OpDecorate" && args.size() >= 2) {
            SpvDecorations& decor = decorations[std::string(args[0])];
            if (args[1] == "DescriptorSet" && args.size() >= 3) {
                decor.set = parse_u32(args[2]);
            } else if (args[1] == "Binding" && args.size() >= 3) {
                decor.binding = parse_u32(args[2]);
            } else if (args[1] == "NonWritable") {
                decor.non_writable = true;
            }
        } else if (op == "OpMemberDecorate" && args.size() >= 3) {
            const std::string type_id(args[0]);
            const auto index = parse_u32(args[1]);
            if (!index) {
                continue;
            }
            if (args[2] == "Offset" && args.size() >= 4) {
                if (const auto offset = parse_u32(args[3])) {
                    member_offsets[type_id][*index] = *offset;
                }
            } else if (args[2] == "NonWritable") {
                member_non_writable[type_id].insert(*index);
            }
        } else if (op == "OpConstant" && args.size() >= 2 && !result.empty()) {
            if (const auto value = parse_u32(args[1])) {
                constants[result] = *value;
            }
        } else if (op == "OpVariable" && args.size() >= 2 && !result.empty()) {
            variables[result] = SpvVariable{std::string(args[0]), std::string(args[1])};
        } else if (!result.empty() && op.rfind("OpType", 0) == 0) {
            SpvType type;
            type.op = std::string(op);
            for (const auto& arg : args) {
                type.operands.emplace_back(arg);
            }
            types[result] = std::move(type);
        }
    }

    ShaderReflection reflection;
    reflection.target = ShaderTarget::SpirV;
    for (const auto& [id, variable] : variables) {
        const auto decor_it = decorations.find(id);
        if (decor_it == decorations.end() || !decor_it->second.set || !decor_it->second.binding) {
            continue;
        }
        const SpvDecorations& decor = decor_it->second;
        if (variable.storage != "Uniform" && variable.storage != "StorageBuffer" && variable.storage != "UniformConstant") {
            continue;
        }

        // The variable's pointer type names the resource. Arrays are unwrapped into a count.
        const auto ptr = types.find(variable.type);
        if (ptr == types.end() || ptr->second.op != "OpTypePointer" || ptr->second.operands.size() < 2) {
            return core::failure("variable " + id + " has no pointer type");
        }
        std::string pointee = ptr->second.operands[1];
        std::uint32_t count = 1;
        while (types.count(pointee) != 0 && types.at(pointee).op == "OpTypeArray") {
            const SpvType& array = types.at(pointee);
            const auto length = array.operands.size() >= 2 ? constants.find(array.operands[1]) : constants.end();
            if (length == constants.end()) {
                return core::failure("array length of " + id + " is not a constant");
            }
            count *= length->second;
            pointee = array.operands[0];
        }
        if (types.count(pointee) != 0 && types.at(pointee).op == "OpTypeRuntimeArray") {
            return core::failure("unbounded descriptor array '" + id + "' is not supported by the descriptor model");
        }
        const auto resolved = types.find(pointee);
        if (resolved == types.end()) {
            return core::failure("variable " + id + " points at an unknown type");
        }
        const std::string& kind = resolved->second.op;

        ReflectedBinding binding;
        binding.name = names.count(id) != 0 ? names.at(id) : id;
        binding.set = *decor.set;
        binding.binding = *decor.binding;
        binding.count = count;

        if (variable.storage == "Uniform" && kind == "OpTypeStruct") {
            binding.cls = BindingClass::ConstantBuffer;
            ReflectedBlock block;
            block.name = binding.name;
            block.set = binding.set;
            block.binding = binding.binding;
            const std::size_t member_count = resolved->second.operands.size();
            for (std::uint32_t index = 0; index < member_count; ++index) {
                ReflectedMember member;
                const auto names_of = member_names.find(pointee);
                member.name = names_of != member_names.end() && names_of->second.count(index) != 0 ? names_of->second.at(index)
                                                                                                  : "member" + std::to_string(index);
                const auto offsets_of = member_offsets.find(pointee);
                member.offset = offsets_of != member_offsets.end() && offsets_of->second.count(index) != 0
                                    ? offsets_of->second.at(index)
                                    : 0;
                block.members.push_back(std::move(member));
            }
            reflection.blocks.push_back(std::move(block));
        } else if (variable.storage == "StorageBuffer" && kind == "OpTypeStruct") {
            const auto non_writable = member_non_writable.find(pointee);
            const bool read_only = decor.non_writable || (non_writable != member_non_writable.end() && !non_writable->second.empty());
            binding.cls = read_only ? BindingClass::ShaderResource : BindingClass::UnorderedAccess;
        } else if (variable.storage == "UniformConstant" && kind == "OpTypeImage") {
            // Operand 5 is Sampled: 1 = read through a sampler, 2 = read/write storage image.
            const std::string sampled = resolved->second.operands.size() > 5 ? resolved->second.operands[5] : "0";
            binding.cls = sampled == "2" ? BindingClass::UnorderedAccess : BindingClass::ShaderResource;
        } else if (variable.storage == "UniformConstant" && (kind == "OpTypeSampledImage")) {
            binding.cls = BindingClass::ShaderResource;
        } else if (variable.storage == "UniformConstant" && kind == "OpTypeSampler") {
            binding.cls = BindingClass::Sampler;
        } else {
            return core::failure("unsupported SPIR-V resource '" + binding.name + "' (" + kind + " in " + variable.storage + ")");
        }
        reflection.bindings.push_back(binding);
    }
    sort_reflection(reflection);
    return core::success(std::move(reflection));
}

// ---- Validation ---------------------------------------------------------------------------------------------------------

namespace {

std::optional<rhi::DescriptorType> descriptor_type_of(BindingClass cls) {
    switch (cls) {
        case BindingClass::ConstantBuffer:
            return rhi::DescriptorType::ConstantBuffer;
        case BindingClass::ShaderResource:
            return rhi::DescriptorType::ShaderResource;
        case BindingClass::UnorderedAccess:
            return rhi::DescriptorType::UnorderedAccess;
        case BindingClass::Sampler:
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

std::string_view to_string(ShaderTarget target) noexcept {
    return target == ShaderTarget::Dxil ? "dxil" : "spirv";
}

std::string_view to_string(BindingClass cls) noexcept {
    switch (cls) {
        case BindingClass::ConstantBuffer:
            return "constant buffer";
        case BindingClass::ShaderResource:
            return "shader resource";
        case BindingClass::UnorderedAccess:
            return "unordered access";
        case BindingClass::Sampler:
            return "sampler";
    }
    return "unknown";
}

std::vector<Diagnostic> validate_bindings(const std::string& source, const ShaderReflection& reflection,
                                         const std::vector<rhi::DescriptorSetLayoutDesc>& layouts) {
    std::vector<Diagnostic> out;
    for (const ReflectedBinding& binding : reflection.bindings) {
        const std::string where = describe_binding(binding);
        if (binding.cls == BindingClass::Sampler) {
            out.push_back(error_at(source, where + ": samplers are not in the descriptor model yet"));
            continue;
        }
        if (binding.set >= layouts.size()) {
            out.push_back(error_at(source, where + ": set " + std::to_string(binding.set) + " has no declared layout"));
            continue;
        }
        const auto& declared = layouts[binding.set].bindings;
        const auto found = std::find_if(declared.begin(), declared.end(), [&](const rhi::DescriptorBindingDesc& entry) {
            return entry.binding == binding.binding;
        });
        if (found == declared.end()) {
            out.push_back(error_at(source, where + ": the layout of set " + std::to_string(binding.set) + " does not declare it"));
            continue;
        }
        if (descriptor_type_of(binding.cls) != found->type) {
            out.push_back(error_at(source, where + ": the layout declares it as a different descriptor type than the shader uses (" +
                                              std::string(to_string(binding.cls)) + ")"));
        }
        if (found->count != binding.count) {
            out.push_back(error_at(source, where + ": the layout declares " + std::to_string(found->count) +
                                              " descriptor(s) but the shader uses " + std::to_string(binding.count)));
        }
    }
    return out;
}

std::vector<Diagnostic> validate_cpu_layouts(const std::string& source, const ShaderReflection& reflection,
                                            const std::vector<CpuBlockLayout>& declared) {
    std::vector<Diagnostic> out;
    for (const CpuBlockLayout& cpu : declared) {
        const auto block = std::find_if(reflection.blocks.begin(), reflection.blocks.end(),
                                        [&](const ReflectedBlock& candidate) { return candidate.name == cpu.name; });
        if (block == reflection.blocks.end()) {
            out.push_back(error_at(source, "constant buffer '" + cpu.name + "' is not in the shader"));
            continue;
        }
        if (block->set != cpu.set || block->binding != cpu.binding) {
            out.push_back(error_at(source, "constant buffer '" + cpu.name + "' is at set " + std::to_string(block->set) +
                                              ", binding " + std::to_string(block->binding) + " in the shader but set " +
                                              std::to_string(cpu.set) + ", binding " + std::to_string(cpu.binding) + " on the CPU"));
        }
        if (block->size_bytes != 0 && cpu.size_bytes != block->size_bytes) {
            out.push_back(error_at(source, "constant buffer '" + cpu.name + "' is " + std::to_string(block->size_bytes) +
                                              " bytes in the shader but " + std::to_string(cpu.size_bytes) + " on the CPU"));
        }
        for (const ReflectedMember& member : block->members) {
            const auto mine = std::find_if(cpu.members.begin(), cpu.members.end(),
                                           [&](const ReflectedMember& candidate) { return candidate.name == member.name; });
            if (mine == cpu.members.end()) {
                out.push_back(error_at(source, cpu.name + "." + member.name + " is missing from the CPU struct"));
            } else if (mine->offset != member.offset) {
                out.push_back(error_at(source, cpu.name + "." + member.name + " is at offset " + std::to_string(member.offset) +
                                                  " in the shader but " + std::to_string(mine->offset) + " on the CPU"));
            }
        }
        for (const ReflectedMember& member : cpu.members) {
            const bool exists = std::any_of(block->members.begin(), block->members.end(),
                                            [&](const ReflectedMember& candidate) { return candidate.name == member.name; });
            if (!exists) {
                out.push_back(error_at(source, cpu.name + "." + member.name + " does not exist in the shader"));
            }
        }
    }
    for (const ReflectedBlock& block : reflection.blocks) {
        const bool declared_here = std::any_of(declared.begin(), declared.end(),
                                               [&](const CpuBlockLayout& cpu) { return cpu.name == block.name; });
        if (!declared_here) {
            out.push_back(error_at(source, "constant buffer '" + block.name + "' has no CPU layout"));
        }
    }
    return out;
}

std::vector<Diagnostic> compare_reflections(const std::string& source, const ShaderReflection& lhs, const ShaderReflection& rhs) {
    std::vector<Diagnostic> out;
    const std::string pair = std::string(to_string(lhs.target)) + " and " + std::string(to_string(rhs.target));
    for (const ReflectedBinding& binding : lhs.bindings) {
        const bool matched = std::any_of(rhs.bindings.begin(), rhs.bindings.end(),
                                         [&](const ReflectedBinding& other) { return other == binding; });
        if (!matched) {
            out.push_back(error_at(source, pair + " disagree on " + describe_binding(binding)));
        }
    }
    for (const ReflectedBinding& binding : rhs.bindings) {
        const bool matched = std::any_of(lhs.bindings.begin(), lhs.bindings.end(),
                                         [&](const ReflectedBinding& other) { return other == binding; });
        if (!matched) {
            out.push_back(error_at(source, pair + " disagree on " + describe_binding(binding)));
        }
    }
    if (lhs.blocks.size() != rhs.blocks.size()) {
        out.push_back(error_at(source, pair + " report different numbers of constant buffers"));
        return out;
    }
    for (std::size_t i = 0; i < lhs.blocks.size(); ++i) {
        const ReflectedBlock& a = lhs.blocks[i];
        const ReflectedBlock& b = rhs.blocks[i];
        if (a.set != b.set || a.binding != b.binding || a.members.size() != b.members.size()) {
            out.push_back(error_at(source, pair + " disagree on constant buffer '" + a.name + "'"));
            continue;
        }
        for (std::size_t m = 0; m < a.members.size(); ++m) {
            if (a.members[m].name != b.members[m].name || a.members[m].offset != b.members[m].offset) {
                out.push_back(error_at(source, pair + " disagree on the layout of " + a.name + "." + a.members[m].name));
            }
        }
    }
    return out;
}

}  // namespace engine::shader
