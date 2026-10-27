// JSON export of a compiled render graph (US-032). The output is for diagnostics and tests; it is not a save format.

#include "engine/render/render_graph.hpp"

#include <cstdio>
#include <string>

namespace engine::render {
namespace {

std::string quote(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += escaped;
                } else {
                    out += c;
                }
                break;
        }
    }
    out += '"';
    return out;
}

std::string number(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    return buffer;
}

std::string optional_number(const std::optional<double>& value) {
    return value.has_value() ? number(*value) : std::string("null");
}

std::string_view kind_name(ResourceKind kind) noexcept {
    return kind == ResourceKind::Texture ? "texture" : "buffer";
}

}  // namespace

std::string export_json(const CompiledGraph& graph, const ExecutionReport* report) {
    // Edges and lifetimes refer to declaration indices, so names are looked up through the passes' declaration order.
    std::vector<std::string> names_by_declaration(graph.passes.size());
    for (const CompiledPass& pass : graph.passes) {
        names_by_declaration[pass.declaration_index] = pass.name;
    }
    const auto barrier_json = [&graph](const Barrier& barrier) {
        return "{\"slot\": " + std::to_string(barrier.slot) + ", \"name\": " + quote(graph.slots[barrier.slot].name) +
               ", \"before\": " + quote(to_string(barrier.before)) + ", \"after\": " + quote(to_string(barrier.after)) + "}";
    };

    std::string out = "{\n";
    out += "  \"schema\": \"engine.render_graph\",\n";
    out += "  \"version\": 1,\n";

    out += "  \"passes\": [";
    for (std::size_t i = 0; i < graph.passes.size(); ++i) {
        const CompiledPass& pass = graph.passes[i];
        out += i == 0 ? "\n" : ",\n";
        out += "    {\"index\": " + std::to_string(i) + ", \"name\": " + quote(pass.name) + ", \"kind\": " +
               quote(to_string(pass.kind)) + ", \"queue\": \"graphics\", \"accesses\": [";
        for (std::size_t a = 0; a < pass.accesses.size(); ++a) {
            const ResourceAccess& access = pass.accesses[a];
            out += a == 0 ? "" : ", ";
            out += "{\"resource\": " + quote(graph.resources[access.resource.index].name) + ", \"access\": " +
                   quote(to_string(access.access)) + ", \"write\": " + (is_write(access.access) ? "true" : "false") + "}";
        }
        out += "], \"barriers\": [";
        for (std::size_t b = 0; b < pass.barriers.size(); ++b) {
            out += b == 0 ? "" : ", ";
            out += barrier_json(pass.barriers[b]);
        }
        out += "]";
        if (report != nullptr && i < report->passes.size()) {
            out += ", \"cpu_ms\": " + number(report->passes[i].cpu_ms) + ", \"gpu_ms\": " + optional_number(report->passes[i].gpu_ms);
        }
        out += "}";
    }
    out += graph.passes.empty() ? "],\n" : "\n  ],\n";

    out += "  \"edges\": [";
    for (std::size_t e = 0; e < graph.edges.size(); ++e) {
        out += e == 0 ? "" : ", ";
        out += "{\"from\": " + quote(names_by_declaration[graph.edges[e].from]) + ", \"to\": " +
               quote(names_by_declaration[graph.edges[e].to]) + "}";
    }
    out += "],\n";

    out += "  \"resources\": [";
    for (std::size_t r = 0; r < graph.resources.size(); ++r) {
        const GraphResource& resource = graph.resources[r];
        const ResourceLifetime& life = graph.lifetimes[r];
        out += r == 0 ? "\n" : ",\n";
        out += "    {\"name\": " + quote(resource.name) + ", \"kind\": \"" + std::string(kind_name(resource.kind)) +
               "\", \"imported\": " + (resource.imported ? "true" : "false") + ", \"used\": " + (life.used ? "true" : "false");
        if (life.used) {
            out += ", \"slot\": " + std::to_string(life.slot) + ", \"first_pass\": " + std::to_string(life.first_pass) +
                   ", \"last_pass\": " + std::to_string(life.last_pass);
        }
        out += "}";
    }
    out += graph.resources.empty() ? "],\n" : "\n  ],\n";

    out += "  \"slots\": [";
    for (std::size_t s = 0; s < graph.slots.size(); ++s) {
        const PhysicalSlot& slot = graph.slots[s];
        out += s == 0 ? "" : ", ";
        out += "{\"index\": " + std::to_string(s) + ", \"name\": " + quote(slot.name) + ", \"kind\": \"" +
               std::string(kind_name(slot.kind)) + "\", \"imported\": " + (slot.imported ? "true" : "false") + "}";
    }
    out += "],\n";

    out += "  \"exit_barriers\": [";
    for (std::size_t b = 0; b < graph.exit_barriers.size(); ++b) {
        out += b == 0 ? "" : ", ";
        out += barrier_json(graph.exit_barriers[b]);
    }
    out += "]";

    if (report != nullptr) {
        out += ",\n  \"timings\": {\"cpu_ms\": " + number(report->cpu_ms) + ", \"gpu_ms\": " + optional_number(report->gpu_ms) +
               ", \"barriers\": " + std::to_string(report->barriers) +
               ", \"transient_allocations\": " + std::to_string(report->transient_allocations) +
               ", \"aliased_resources\": " + std::to_string(report->aliased_resources) + "}";
    }
    out += "\n}\n";
    return out;
}

}  // namespace engine::render
