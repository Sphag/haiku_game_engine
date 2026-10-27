// Render graph compilation (US-032): validation, ordering, lifetimes, transient slot assignment, and barriers. Nothing here touches a
// device, so every rule is testable on any operating system.

#include "engine/render/render_graph.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace engine::render {
namespace {

constexpr std::uint32_t kNone = ResourceId::kInvalidIndex;

bool buffer_only(Access access) noexcept {
    return access == Access::VertexRead || access == Access::IndexRead || access == Access::ConstantRead ||
           access == Access::IndirectRead;
}

bool texture_only(Access access) noexcept {
    return access == Access::ColorWrite || access == Access::DepthWrite || access == Access::DepthRead;
}

// Accesses that only the graphics queue can perform in this version. Compute passes may not use them.
bool graphics_only(Access access) noexcept {
    return access == Access::VertexRead || access == Access::IndexRead || access == Access::IndirectRead ||
           access == Access::ColorWrite || access == Access::DepthWrite || access == Access::DepthRead;
}

bool same_texture_layout(const rhi::TextureDesc& lhs, const rhi::TextureDesc& rhs) noexcept {
    return lhs.width == rhs.width && lhs.height == rhs.height && lhs.mip_levels == rhs.mip_levels && lhs.format == rhs.format &&
           lhs.usage == rhs.usage;
}

bool same_buffer_layout(const rhi::BufferDesc& lhs, const rhi::BufferDesc& rhs) noexcept {
    return lhs.size_bytes == rhs.size_bytes && lhs.usage == rhs.usage;
}

bool slot_matches(const PhysicalSlot& slot, const GraphResource& resource) noexcept {
    if (slot.kind != resource.kind) {
        return false;
    }
    return resource.kind == ResourceKind::Texture ? same_texture_layout(slot.texture, resource.texture)
                                                  : same_buffer_layout(slot.buffer, resource.buffer);
}

GraphError make_error(GraphErrorKind kind, std::vector<std::string> passes, std::string resource, std::string message) {
    GraphError error;
    error.kind = kind;
    error.passes = std::move(passes);
    error.resource = std::move(resource);
    error.message = std::move(message);
    return error;
}

// Walks backward along unprocessed predecessors until a node repeats. Every unprocessed node has an unprocessed predecessor, so the
// walk never stops early. The loop found is returned in dependency order: each entry feeds the next, and the last feeds the first.
std::vector<std::uint32_t> find_cycle(const std::vector<std::vector<std::uint32_t>>& predecessors, const std::vector<bool>& processed) {
    std::uint32_t start = 0;
    while (processed[start]) {
        ++start;
    }
    constexpr std::size_t kUnvisited = static_cast<std::size_t>(-1);
    std::vector<std::uint32_t> walk;
    std::vector<std::size_t> position(processed.size(), kUnvisited);
    std::uint32_t current = start;
    while (position[current] == kUnvisited) {
        position[current] = walk.size();
        walk.push_back(current);
        std::uint32_t next = kNone;
        for (std::uint32_t pred : predecessors[current]) {
            if (!processed[pred]) {
                next = pred;
                break;
            }
        }
        current = next;
    }
    std::vector<std::uint32_t> cycle(walk.begin() + static_cast<std::ptrdiff_t>(position[current]), walk.end());
    std::reverse(cycle.begin(), cycle.end());
    // Start at the earliest-declared pass, so the same cycle always reads the same way.
    std::rotate(cycle.begin(), std::min_element(cycle.begin(), cycle.end()), cycle.end());
    return cycle;
}

}  // namespace

std::string_view to_string(PassKind kind) noexcept {
    switch (kind) {
        case PassKind::Graphics:
            return "graphics";
        case PassKind::Compute:
            return "compute";
    }
    return "unknown";
}

std::string_view to_string(Access access) noexcept {
    switch (access) {
        case Access::VertexRead:
            return "VertexRead";
        case Access::IndexRead:
            return "IndexRead";
        case Access::ConstantRead:
            return "ConstantRead";
        case Access::ShaderRead:
            return "ShaderRead";
        case Access::IndirectRead:
            return "IndirectRead";
        case Access::StorageWrite:
            return "StorageWrite";
        case Access::ColorWrite:
            return "ColorWrite";
        case Access::DepthWrite:
            return "DepthWrite";
        case Access::DepthRead:
            return "DepthRead";
        case Access::CopySource:
            return "CopySource";
        case Access::CopyDestination:
            return "CopyDestination";
    }
    return "unknown";
}

bool is_write(Access access) noexcept {
    return access == Access::StorageWrite || access == Access::ColorWrite || access == Access::DepthWrite ||
           access == Access::CopyDestination;
}

rhi::ResourceState required_state(Access access) noexcept {
    switch (access) {
        case Access::VertexRead:
            return rhi::ResourceState::VertexBuffer;
        case Access::IndexRead:
            return rhi::ResourceState::IndexBuffer;
        case Access::ConstantRead:
            return rhi::ResourceState::ConstantBuffer;
        case Access::ShaderRead:
            return rhi::ResourceState::ShaderRead;
        case Access::IndirectRead:
            return rhi::ResourceState::IndirectArgument;
        case Access::StorageWrite:
            return rhi::ResourceState::UnorderedAccess;
        case Access::ColorWrite:
            return rhi::ResourceState::RenderTarget;
        case Access::DepthWrite:
            return rhi::ResourceState::DepthWrite;
        case Access::DepthRead:
            return rhi::ResourceState::DepthRead;
        case Access::CopySource:
            return rhi::ResourceState::CopySource;
        case Access::CopyDestination:
            return rhi::ResourceState::CopyDestination;
    }
    return rhi::ResourceState::Common;
}

std::string_view to_string(rhi::ResourceState state) noexcept {
    switch (state) {
        case rhi::ResourceState::Common:
            return "Common";
        case rhi::ResourceState::VertexBuffer:
            return "VertexBuffer";
        case rhi::ResourceState::IndexBuffer:
            return "IndexBuffer";
        case rhi::ResourceState::ConstantBuffer:
            return "ConstantBuffer";
        case rhi::ResourceState::ShaderRead:
            return "ShaderRead";
        case rhi::ResourceState::UnorderedAccess:
            return "UnorderedAccess";
        case rhi::ResourceState::RenderTarget:
            return "RenderTarget";
        case rhi::ResourceState::DepthWrite:
            return "DepthWrite";
        case rhi::ResourceState::DepthRead:
            return "DepthRead";
        case rhi::ResourceState::IndirectArgument:
            return "IndirectArgument";
        case rhi::ResourceState::CopySource:
            return "CopySource";
        case rhi::ResourceState::CopyDestination:
            return "CopyDestination";
        case rhi::ResourceState::Present:
            return "Present";
    }
    return "unknown";
}

std::string_view to_string(GraphErrorKind kind) noexcept {
    switch (kind) {
        case GraphErrorKind::DuplicateName:
            return "duplicate name";
        case GraphErrorKind::InvalidResource:
            return "invalid resource";
        case GraphErrorKind::IncompatibleUsage:
            return "incompatible usage";
        case GraphErrorKind::MultipleWriters:
            return "multiple writers";
        case GraphErrorKind::ReadBeforeWrite:
            return "read before write";
        case GraphErrorKind::Cycle:
            return "cycle";
    }
    return "unknown";
}

ResourceId RenderGraph::create_texture(std::string name, rhi::TextureDesc desc) {
    GraphResource resource;
    resource.name = std::move(name);
    resource.kind = ResourceKind::Texture;
    resource.texture = std::move(desc);
    resources_.push_back(std::move(resource));
    return ResourceId{static_cast<std::uint32_t>(resources_.size() - 1)};
}

ResourceId RenderGraph::create_buffer(std::string name, rhi::BufferDesc desc) {
    GraphResource resource;
    resource.name = std::move(name);
    resource.kind = ResourceKind::Buffer;
    resource.buffer = std::move(desc);
    resources_.push_back(std::move(resource));
    return ResourceId{static_cast<std::uint32_t>(resources_.size() - 1)};
}

ResourceId RenderGraph::import_texture(std::string name, rhi::TextureHandle handle, rhi::TextureDesc usage, rhi::ResourceState initial,
                                       std::optional<rhi::ResourceState> final_state) {
    GraphResource resource;
    resource.name = std::move(name);
    resource.kind = ResourceKind::Texture;
    resource.imported = true;
    resource.texture = std::move(usage);
    resource.imported_handles.texture = handle;
    resource.initial_state = initial;
    resource.final_state = final_state;
    resources_.push_back(std::move(resource));
    return ResourceId{static_cast<std::uint32_t>(resources_.size() - 1)};
}

ResourceId RenderGraph::import_buffer(std::string name, rhi::BufferHandle handle, rhi::BufferDesc usage, rhi::ResourceState initial,
                                      std::optional<rhi::ResourceState> final_state) {
    GraphResource resource;
    resource.name = std::move(name);
    resource.kind = ResourceKind::Buffer;
    resource.imported = true;
    resource.buffer = std::move(usage);
    resource.imported_handles.buffer = handle;
    resource.initial_state = initial;
    resource.final_state = final_state;
    resources_.push_back(std::move(resource));
    return ResourceId{static_cast<std::uint32_t>(resources_.size() - 1)};
}

void RenderGraph::add_pass(std::string name, PassKind kind, std::vector<ResourceAccess> accesses, PassRecordFn record) {
    PassDesc pass;
    pass.name = std::move(name);
    pass.kind = kind;
    pass.accesses = std::move(accesses);
    pass.record = std::move(record);
    passes_.push_back(std::move(pass));
}

CompileResult RenderGraph::compile() const {
    CompileResult result;
    std::vector<GraphError>& errors = result.errors;
    const std::size_t pass_count = passes_.size();
    const std::size_t resource_count = resources_.size();

    std::set<std::string_view> pass_names;
    for (const PassDesc& pass : passes_) {
        if (!pass_names.insert(pass.name).second) {
            errors.push_back(make_error(GraphErrorKind::DuplicateName, {pass.name}, "",
                                        "duplicate name: more than one pass is called '" + pass.name + "'"));
        }
    }
    std::set<std::string_view> resource_names;
    for (const GraphResource& resource : resources_) {
        if (!resource_names.insert(resource.name).second) {
            errors.push_back(make_error(GraphErrorKind::DuplicateName, {}, resource.name,
                                        "duplicate name: more than one resource is called '" + resource.name + "'"));
        }
        if (resource.imported) {
            const bool valid = resource.kind == ResourceKind::Texture ? resource.imported_handles.texture.valid()
                                                                       : resource.imported_handles.buffer.valid();
            if (!valid) {
                errors.push_back(make_error(GraphErrorKind::InvalidResource, {}, resource.name,
                                            "invalid resource: imported '" + resource.name + "' has an invalid handle"));
            }
        }
    }

    // Checks each access, and records the single writer and the readers of every resource.
    std::vector<std::uint32_t> writer(resource_count, kNone);
    std::vector<std::vector<std::uint32_t>> readers(resource_count);
    for (std::uint32_t p = 0; p < pass_count; ++p) {
        const PassDesc& pass = passes_[p];
        std::vector<std::uint32_t> declared;
        for (const ResourceAccess& access : pass.accesses) {
            if (!access.resource.valid() || access.resource.index >= resource_count) {
                errors.push_back(make_error(GraphErrorKind::InvalidResource, {pass.name}, "",
                                            "invalid resource: pass '" + pass.name + "' accesses a resource this graph did not create"));
                continue;
            }
            const std::uint32_t r = access.resource.index;
            const GraphResource& resource = resources_[r];
            if (std::find(declared.begin(), declared.end(), r) != declared.end()) {
                errors.push_back(make_error(GraphErrorKind::IncompatibleUsage, {pass.name}, resource.name,
                                            "incompatible usage: pass '" + pass.name + "' declares '" + resource.name +
                                                "' more than once"));
                continue;
            }
            declared.push_back(r);

            if (pass.kind == PassKind::Compute && graphics_only(access.access)) {
                errors.push_back(make_error(GraphErrorKind::IncompatibleUsage, {pass.name}, resource.name,
                                            "incompatible usage: compute pass '" + pass.name + "' cannot use '" +
                                                std::string(to_string(access.access)) + "' on '" + resource.name +
                                                "', which needs a graphics pass"));
                continue;
            }
            const bool kind_matches = resource.kind == ResourceKind::Texture ? !buffer_only(access.access)
                                                                             : !texture_only(access.access);
            if (!kind_matches) {
                errors.push_back(make_error(GraphErrorKind::IncompatibleUsage, {pass.name}, resource.name,
                                            "incompatible usage: pass '" + pass.name + "' uses " +
                                                (resource.kind == ResourceKind::Texture ? "texture" : "buffer") + " '" +
                                                resource.name + "' with access '" + std::string(to_string(access.access)) + "'"));
                continue;
            }
            const rhi::ResourceState state = required_state(access.access);
            const bool allowed = resource.kind == ResourceKind::Texture ? rhi::texture_state_allowed(resource.texture.usage, state)
                                                                        : rhi::buffer_state_allowed(resource.buffer.usage, state);
            if (!allowed) {
                errors.push_back(make_error(GraphErrorKind::IncompatibleUsage, {pass.name}, resource.name,
                                            "incompatible usage: pass '" + pass.name + "' needs '" + resource.name + "' to allow " +
                                                std::string(to_string(state)) + ", and its usage flags do not"));
                continue;
            }

            if (is_write(access.access)) {
                if (writer[r] != kNone) {
                    errors.push_back(make_error(GraphErrorKind::MultipleWriters, {passes_[writer[r]].name, pass.name}, resource.name,
                                                "multiple writers: '" + passes_[writer[r]].name + "' and '" + pass.name +
                                                    "' both write '" + resource.name + "'"));
                } else {
                    writer[r] = p;
                }
            } else {
                readers[r].push_back(p);
            }
        }
    }

    for (std::uint32_t r = 0; r < resource_count; ++r) {
        if (resources_[r].imported || writer[r] != kNone) {
            continue;
        }
        for (std::uint32_t reader : readers[r]) {
            errors.push_back(make_error(GraphErrorKind::ReadBeforeWrite, {passes_[reader].name}, resources_[r].name,
                                        "read before write: pass '" + passes_[reader].name + "' reads '" + resources_[r].name +
                                            "', but no pass writes it"));
        }
    }

    // Dependencies run from the writer of a resource to each of its readers.
    std::set<std::pair<std::uint32_t, std::uint32_t>> edge_set;
    std::vector<std::vector<std::uint32_t>> successors(pass_count);
    std::vector<std::vector<std::uint32_t>> predecessors(pass_count);
    for (std::uint32_t r = 0; r < resource_count; ++r) {
        if (writer[r] == kNone) {
            continue;
        }
        for (std::uint32_t reader : readers[r]) {
            if (edge_set.insert({writer[r], reader}).second) {
                successors[writer[r]].push_back(reader);
                predecessors[reader].push_back(writer[r]);
            }
        }
    }

    // Kahn's algorithm. The smallest ready declaration index goes first, so the order is stable for a given declaration.
    std::vector<std::uint32_t> pending(pass_count);
    for (std::uint32_t p = 0; p < pass_count; ++p) {
        pending[p] = static_cast<std::uint32_t>(predecessors[p].size());
    }
    std::set<std::uint32_t> ready;
    for (std::uint32_t p = 0; p < pass_count; ++p) {
        if (pending[p] == 0) {
            ready.insert(p);
        }
    }
    std::vector<std::uint32_t> order;
    std::vector<bool> processed(pass_count, false);
    while (!ready.empty()) {
        const std::uint32_t u = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(u);
        processed[u] = true;
        for (std::uint32_t s : successors[u]) {
            if (--pending[s] == 0) {
                ready.insert(s);
            }
        }
    }
    if (order.size() < pass_count) {
        std::vector<std::string> names;
        std::string path;
        for (std::uint32_t p : find_cycle(predecessors, processed)) {
            names.push_back(passes_[p].name);
            path += "'" + passes_[p].name + "' -> ";
        }
        path += "'" + names.front() + "'";
        errors.push_back(make_error(GraphErrorKind::Cycle, names, "", "cycle: " + path));
    }
    if (!errors.empty()) {
        return result;
    }

    // Lifetimes are spans of execution positions. A pass that accesses a resource keeps it alive from its first access to its last.
    std::vector<ResourceLifetime> lifetimes(resource_count);
    for (std::uint32_t position = 0; position < pass_count; ++position) {
        for (const ResourceAccess& access : passes_[order[position]].accesses) {
            ResourceLifetime& life = lifetimes[access.resource.index];
            if (!life.used) {
                life.used = true;
                life.first_pass = position;
            }
            life.last_pass = position;
        }
    }

    CompiledGraph graph;
    graph.resources = resources_;

    // Imported resources always own a slot. Transient resources share a slot with an earlier one of the same layout when the earlier
    // one's last use comes before this one's first use. Resources are placed in order of first use, which keeps the result stable.
    std::vector<std::uint32_t> slot_last_use;
    for (std::uint32_t r = 0; r < resource_count; ++r) {
        const GraphResource& resource = resources_[r];
        if (!resource.imported) {
            continue;
        }
        PhysicalSlot slot;
        slot.kind = resource.kind;
        slot.imported = true;
        slot.name = resource.name;
        slot.imported_handles = resource.imported_handles;
        slot.initial_state = resource.initial_state;
        graph.slots.push_back(std::move(slot));
        slot_last_use.push_back(0);
        lifetimes[r].slot = static_cast<std::uint32_t>(graph.slots.size() - 1);
    }

    std::vector<std::uint32_t> transients;
    for (std::uint32_t r = 0; r < resource_count; ++r) {
        if (!resources_[r].imported && lifetimes[r].used) {
            transients.push_back(r);
        }
    }
    std::sort(transients.begin(), transients.end(), [&lifetimes](std::uint32_t lhs, std::uint32_t rhs) {
        if (lifetimes[lhs].first_pass != lifetimes[rhs].first_pass) {
            return lifetimes[lhs].first_pass < lifetimes[rhs].first_pass;
        }
        return lhs < rhs;
    });
    for (std::uint32_t r : transients) {
        const GraphResource& resource = resources_[r];
        const ResourceLifetime& life = lifetimes[r];
        std::uint32_t chosen = kNone;
        for (std::uint32_t s = 0; s < graph.slots.size(); ++s) {
            if (!graph.slots[s].imported && slot_last_use[s] < life.first_pass && slot_matches(graph.slots[s], resource)) {
                chosen = s;
                break;
            }
        }
        if (chosen == kNone) {
            PhysicalSlot slot;
            slot.kind = resource.kind;
            slot.name = resource.name;
            slot.texture = resource.texture;
            slot.buffer = resource.buffer;
            slot.initial_state = rhi::ResourceState::Common;
            graph.slots.push_back(std::move(slot));
            slot_last_use.push_back(life.last_pass);
            chosen = static_cast<std::uint32_t>(graph.slots.size() - 1);
        } else {
            slot_last_use[chosen] = life.last_pass;
        }
        lifetimes[r].slot = chosen;
    }
    graph.lifetimes = lifetimes;

    // Barriers follow the execution order. Each slot's state carries from one user to the next, so a reused slot starts from the
    // state its previous user left it in.
    std::vector<rhi::ResourceState> state;
    state.reserve(graph.slots.size());
    for (const PhysicalSlot& slot : graph.slots) {
        state.push_back(slot.initial_state);
    }
    graph.passes.reserve(pass_count);
    for (std::uint32_t position = 0; position < pass_count; ++position) {
        const PassDesc& source = passes_[order[position]];
        CompiledPass pass;
        pass.name = source.name;
        pass.kind = source.kind;
        pass.declaration_index = order[position];
        pass.accesses = source.accesses;
        pass.record = source.record;
        for (const ResourceAccess& access : source.accesses) {
            const std::uint32_t slot = lifetimes[access.resource.index].slot;
            const rhi::ResourceState required = required_state(access.access);
            if (state[slot] != required) {
                pass.barriers.push_back(Barrier{slot, state[slot], required});
                state[slot] = required;
            }
        }
        graph.passes.push_back(std::move(pass));
    }

    for (std::uint32_t r = 0; r < resource_count; ++r) {
        const GraphResource& resource = resources_[r];
        if (!resource.imported || !resource.final_state.has_value()) {
            continue;
        }
        const std::uint32_t slot = lifetimes[r].slot;
        if (state[slot] != *resource.final_state) {
            graph.exit_barriers.push_back(Barrier{slot, state[slot], *resource.final_state});
        }
    }

    for (std::uint32_t u = 0; u < pass_count; ++u) {
        for (std::uint32_t s : successors[u]) {
            graph.edges.push_back(GraphEdge{u, s});
        }
    }
    std::sort(graph.edges.begin(), graph.edges.end(), [](const GraphEdge& lhs, const GraphEdge& rhs) {
        return lhs.from != rhs.from ? lhs.from < rhs.from : lhs.to < rhs.to;
    });

    result.graph = std::move(graph);
    return result;
}

}  // namespace engine::render
