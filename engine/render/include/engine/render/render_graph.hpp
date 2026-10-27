#pragma once

// Render graph (US-032). A frame is described as passes that name the resources they read and write. compile() checks the
// description, orders the passes, computes the state transitions between them, and assigns transient resources to device
// allocations. execute() creates those allocations, records every pass into one command list, submits it, and reports
// CPU and GPU timings.
//
// Rules that compile() enforces. Each one produces an error that names the passes involved.
//  - A resource has at most one writer in a frame. Every reader depends on that writer, so the order follows the data.
//  - A transient resource that is read needs a writer (read before write). An imported resource starts with its content.
//  - The dependencies must form an acyclic graph (cycle).
//  - Each access must be allowed by the pass kind, by the resource kind, and by the usage flags of the resource.
//
// Scope of this version. Compute passes run on the graphics queue: the RHI shares every resource exclusively, so overlapping
// queues would need ownership transfers that this version does not record. Passes declare what they do, and the graph
// derives the barriers from it. Pipeline binding stays in each pass's record function.

#include "engine/rhi/error.hpp"
#include "engine/rhi/handle.hpp"
#include "engine/rhi/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::rhi {
class CommandList;
class Device;
}  // namespace engine::rhi

namespace engine::render {

enum class PassKind : std::uint8_t {
    Graphics,  // draws, render targets, copies
    Compute,   // dispatches, storage writes, shader reads, copies
};
[[nodiscard]] std::string_view to_string(PassKind kind) noexcept;

// What a pass does to a resource. The access selects the state the resource must be in while the pass runs.
enum class Access : std::uint8_t {
    VertexRead,       // VertexBuffer state (buffers; graphics passes)
    IndexRead,        // IndexBuffer state (buffers; graphics passes)
    ConstantRead,     // ConstantBuffer state (buffers)
    ShaderRead,       // ShaderRead state (buffers and textures)
    IndirectRead,     // IndirectArgument state (buffers; graphics passes)
    StorageWrite,     // UnorderedAccess state (buffers and textures)
    ColorWrite,       // RenderTarget state (textures; graphics passes)
    DepthWrite,       // DepthWrite state (textures; graphics passes)
    DepthRead,        // DepthRead state (textures; graphics passes)
    CopySource,       // CopySource state (buffers and textures)
    CopyDestination,  // CopyDestination state (buffers and textures)
};
[[nodiscard]] std::string_view to_string(Access access) noexcept;
[[nodiscard]] bool is_write(Access access) noexcept;
[[nodiscard]] rhi::ResourceState required_state(Access access) noexcept;
[[nodiscard]] std::string_view to_string(rhi::ResourceState state) noexcept;

enum class ResourceKind : std::uint8_t {
    Texture,
    Buffer,
};

// Index of a resource in its graph. A default-constructed id is invalid.
struct ResourceId {
    static constexpr std::uint32_t kInvalidIndex = ~std::uint32_t{0};
    std::uint32_t index = kInvalidIndex;

    [[nodiscard]] bool valid() const noexcept {
        return index != kInvalidIndex;
    }
    friend bool operator==(ResourceId lhs, ResourceId rhs) noexcept {
        return lhs.index == rhs.index;
    }
};

struct ResourceAccess {
    ResourceId resource;
    Access access = Access::ShaderRead;
};

// Device objects that back one logical resource. Only the member that matches the resource kind is valid.
struct PhysicalResource {
    rhi::TextureHandle texture;
    rhi::BufferHandle buffer;
};

class PassContext;
using PassRecordFn = std::function<rhi::RhiStatus(PassContext&)>;

// Handed to a pass while it records. The pass can only resolve resources it declared.
class PassContext {
public:
    PassContext(rhi::CommandList& commands, const std::vector<ResourceAccess>& accesses,
                const std::vector<std::uint32_t>& resource_slots, const std::vector<ResourceKind>& resource_kinds,
                const std::vector<PhysicalResource>& physical) noexcept;

    [[nodiscard]] rhi::CommandList& commands() const noexcept {
        return *commands_;
    }
    // Empty when the pass does not declare the resource or the resource is not a texture.
    [[nodiscard]] rhi::TextureHandle texture(ResourceId resource) const noexcept;
    // Empty when the pass does not declare the resource or the resource is not a buffer.
    [[nodiscard]] rhi::BufferHandle buffer(ResourceId resource) const noexcept;

private:
    [[nodiscard]] const PhysicalResource* find(ResourceId resource, ResourceKind kind) const noexcept;

    rhi::CommandList* commands_;
    const std::vector<ResourceAccess>* accesses_;
    const std::vector<std::uint32_t>* resource_slots_;
    const std::vector<ResourceKind>* resource_kinds_;
    const std::vector<PhysicalResource>* physical_;
};

enum class GraphErrorKind : std::uint8_t {
    DuplicateName,      // two passes, or two resources, share a name
    InvalidResource,    // an access names no resource of this graph, or an imported handle is invalid
    IncompatibleUsage,  // the pass kind, the resource kind, or the usage flags do not allow the access
    MultipleWriters,    // two passes write the same resource
    ReadBeforeWrite,    // a transient resource is read, and no pass writes it
    Cycle,              // the dependencies form a loop
};
[[nodiscard]] std::string_view to_string(GraphErrorKind kind) noexcept;

struct GraphError {
    GraphErrorKind kind = GraphErrorKind::Cycle;
    std::vector<std::string> passes;  // every pass the error names, in the order reported
    std::string resource;             // empty when the error concerns passes only
    std::string message;              // one line that names the passes and the resource
};

// A state change of one physical resource, recorded before the pass that needs it.
struct Barrier {
    std::uint32_t slot = 0;
    rhi::ResourceState before = rhi::ResourceState::Common;
    rhi::ResourceState after = rhi::ResourceState::Common;
};

// One physical allocation. Transient slots are created at execute() and destroyed when it returns. Several transient resources
// with the same layout and disjoint lifetimes share one slot.
struct PhysicalSlot {
    ResourceKind kind = ResourceKind::Texture;
    bool imported = false;
    std::string name;                 // the first resource that uses the slot
    rhi::TextureDesc texture;         // transient textures
    rhi::BufferDesc buffer;           // transient buffers
    PhysicalResource imported_handles;
    rhi::ResourceState initial_state = rhi::ResourceState::Common;
};

struct GraphResource {
    std::string name;
    ResourceKind kind = ResourceKind::Texture;
    bool imported = false;
    rhi::TextureDesc texture;  // for imported textures: the usage flags the handle allows (dimensions unused)
    rhi::BufferDesc buffer;    // for imported buffers: the usage flags the handle allows (size unused)
    PhysicalResource imported_handles;
    rhi::ResourceState initial_state = rhi::ResourceState::Common;
    std::optional<rhi::ResourceState> final_state;  // imported only: state the handle must be in after the graph
};

// Span of execution positions in which a resource is accessed. `used` is false when no pass accesses it.
struct ResourceLifetime {
    bool used = false;
    std::uint32_t first_pass = 0;
    std::uint32_t last_pass = 0;
    std::uint32_t slot = 0;
};

struct GraphEdge {
    std::uint32_t from = 0;  // declaration index of the writer
    std::uint32_t to = 0;    // declaration index of the reader
};

struct CompiledPass {
    std::string name;
    PassKind kind = PassKind::Graphics;
    std::uint32_t declaration_index = 0;
    std::vector<ResourceAccess> accesses;
    std::vector<Barrier> barriers;  // recorded before the pass
    PassRecordFn record;
};

// Output of compile(). Passes are in execution order; the other lists are indexed by the resource or slot they describe.
struct CompiledGraph {
    std::vector<CompiledPass> passes;
    std::vector<GraphEdge> edges;
    std::vector<GraphResource> resources;
    std::vector<ResourceLifetime> lifetimes;
    std::vector<PhysicalSlot> slots;
    std::vector<Barrier> exit_barriers;  // imported resources that end in a state other than their initial state
};

struct CompileResult {
    std::optional<CompiledGraph> graph;  // present only when there are no errors
    std::vector<GraphError> errors;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }
};

class RenderGraph {
public:
    ResourceId create_texture(std::string name, rhi::TextureDesc desc);
    ResourceId create_buffer(std::string name, rhi::BufferDesc desc);
    // The graph does not own the handle. `usage` is what the handle allows; the passes are checked against it.
    ResourceId import_texture(std::string name, rhi::TextureHandle handle, rhi::TextureDesc usage, rhi::ResourceState initial,
                              std::optional<rhi::ResourceState> final_state = std::nullopt);
    ResourceId import_buffer(std::string name, rhi::BufferHandle handle, rhi::BufferDesc usage, rhi::ResourceState initial,
                             std::optional<rhi::ResourceState> final_state = std::nullopt);

    // Passes are kept in declaration order until compile(); the declaration order only breaks ties between independent passes.
    void add_pass(std::string name, PassKind kind, std::vector<ResourceAccess> accesses, PassRecordFn record = {});

    [[nodiscard]] CompileResult compile() const;

private:
    struct PassDesc {
        std::string name;
        PassKind kind = PassKind::Graphics;
        std::vector<ResourceAccess> accesses;
        PassRecordFn record;
    };

    std::vector<GraphResource> resources_;
    std::vector<PassDesc> passes_;
};

// Timings of one execution, for diagnostics. Passes are in execution order.
struct PassTiming {
    std::string name;
    double cpu_ms = 0.0;             // recording time: barriers and the pass's record function
    std::optional<double> gpu_ms;    // empty when the device has no timestamp queries
};

struct ExecutionReport {
    rhi::Backend backend = rhi::Backend::Validation;
    std::vector<PassTiming> passes;
    double cpu_ms = 0.0;                        // recording time for the whole graph, without submission
    std::optional<double> gpu_ms;               // first pass start to last pass end; empty without timestamp queries
    std::uint32_t barriers = 0;
    std::uint32_t transient_allocations = 0;    // device resources created for this execution
    std::uint32_t aliased_resources = 0;        // transient resources that reused an allocation
};

// Records the compiled graph into one graphics command list, submits it, and waits for it. Transient resources are created first
// and destroyed before returning, so the device holds no resources from the graph afterward. On failure the error names the
// pass that rejected the recording.
[[nodiscard]] rhi::RhiResult<ExecutionReport> execute(rhi::Device& device, const CompiledGraph& graph);

// JSON description of the graph for diagnostics: passes, accesses, barriers, edges, lifetimes, and slots. Timings are included
// when a report from execute() is given.
[[nodiscard]] std::string export_json(const CompiledGraph& graph, const ExecutionReport* report = nullptr);

}  // namespace engine::render
