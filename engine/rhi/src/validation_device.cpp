#include "engine/rhi/validation_device.hpp"

#include "engine/rhi/pipeline_rules.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace engine::rhi {
namespace {

constexpr std::uint64_t kTimestampFrequencyHz = 1'000'000'000;

std::uint32_t next_device_id() {
    static std::atomic<std::uint32_t> next{1};
    return next.fetch_add(1);
}

enum class ResourceKind : std::uint8_t {
    Buffer,
    Texture,
    DescriptorSet,
    Pipeline,
    Fence,
    QueryPool,
};

// Names one object slot at one generation. Submissions keep these to hold objects alive until the GPU is done.
struct ResourceKey {
    ResourceKind kind = ResourceKind::Buffer;
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    friend bool operator==(const ResourceKey& lhs, const ResourceKey& rhs) noexcept {
        return lhs.kind == rhs.kind && lhs.index == rhs.index && lhs.generation == rhs.generation;
    }
    friend bool operator<(const ResourceKey& lhs, const ResourceKey& rhs) noexcept {
        return std::tie(lhs.kind, lhs.index, lhs.generation) < std::tie(rhs.kind, rhs.index, rhs.generation);
    }
};

template <typename Tag>
ResourceKey key_of(ResourceKind kind, const Handle<Tag>& handle) {
    return ResourceKey{kind, handle.index, handle.generation};
}

// A pool of object slots with generations and deferred reuse. A destroyed slot stays allocated while a submission still
// holds it (in_flight > 0). It is reused only after the last holder releases it, so the GPU never sees freed storage.
template <typename Tag, typename Record>
class Pool {
public:
    using HandleType = Handle<Tag>;

    HandleType add(Record record, std::uint32_t device_id) {
        std::uint32_t index = 0;
        if (free_.empty()) {
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        } else {
            index = free_.back();
            free_.pop_back();
        }
        Slot& slot = slots_[index];
        slot.record = std::move(record);
        slot.alive = true;
        return HandleType{index, slot.generation, device_id};
    }

    [[nodiscard]] bool live_index(std::uint32_t index, std::uint32_t generation) const noexcept {
        if (index >= slots_.size()) {
            return false;
        }
        const Slot& slot = slots_[index];
        return slot.alive && !slot.destroy_requested && slot.generation == generation;
    }

    // Returns the record for a live handle of this device, or nullptr.
    [[nodiscard]] Record* find(const HandleType& handle, std::uint32_t device_id) {
        if (handle.device != device_id || !live_index(handle.index, handle.generation)) {
            return nullptr;
        }
        return &slots_[handle.index].record;
    }
    [[nodiscard]] const Record* find(const HandleType& handle, std::uint32_t device_id) const {
        if (handle.device != device_id || !live_index(handle.index, handle.generation)) {
            return nullptr;
        }
        return &slots_[handle.index].record;
    }

    [[nodiscard]] bool live(const HandleType& handle, std::uint32_t device_id) const noexcept {
        return find(handle, device_id) != nullptr;
    }

    // Record for a slot a submission holds. The caller has already checked that the slot is still allocated.
    [[nodiscard]] Record& at(std::uint32_t index) {
        return slots_[index].record;
    }
    [[nodiscard]] const Record& at(std::uint32_t index) const {
        return slots_[index].record;
    }

    void destroy(std::uint32_t index) {
        Slot& slot = slots_[index];
        slot.destroy_requested = true;
        if (slot.in_flight == 0) {
            free_slot(index);
        }
    }

    void retain(std::uint32_t index) {
        ++slots_[index].in_flight;
    }

    void release(std::uint32_t index) {
        Slot& slot = slots_[index];
        if (slot.in_flight > 0) {
            --slot.in_flight;
        }
        if (slot.in_flight == 0 && slot.destroy_requested) {
            free_slot(index);
        }
    }

    [[nodiscard]] std::size_t allocated() const noexcept {
        return static_cast<std::size_t>(std::count_if(slots_.begin(), slots_.end(), [](const Slot& slot) { return slot.alive; }));
    }

private:
    struct Slot {
        Record record{};
        std::uint32_t generation = 1;
        std::uint32_t in_flight = 0;
        bool alive = false;
        bool destroy_requested = false;
    };

    void free_slot(std::uint32_t index) {
        Slot& slot = slots_[index];
        slot.alive = false;
        slot.destroy_requested = false;
        slot.record = Record{};
        slot.generation = slot.generation == std::numeric_limits<std::uint32_t>::max() ? 1 : slot.generation + 1;
        free_.push_back(index);
    }

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
};

struct BufferRecord {
    BufferDesc desc;
    ResourceState state = ResourceState::Common;
};

struct TextureRecord {
    TextureDesc desc;
    ResourceState state = ResourceState::Common;
};

struct DescriptorEntry {
    bool written = false;
    DescriptorType type = DescriptorType::ConstantBuffer;
    ResourceKey key{};
};

struct DescriptorSetRecord {
    std::vector<DescriptorBindingDesc> bindings;
    std::vector<std::uint32_t> first_entry;  // per binding: index of its first array element in `entries`
    std::vector<DescriptorEntry> entries;
};

struct PipelineRecord {
    PipelineKind kind = PipelineKind::Graphics;
    bool per_instance = false;  // reads vertex buffer slot 1 (PipelineDesc::instance_attributes)
    std::vector<std::vector<DescriptorBindingDesc>> layouts;
    std::vector<Format> color_formats;
    Format depth_format = Format::Unknown;
};

struct FenceRecord {
    std::uint64_t completed = 0;
    std::uint64_t signaled = 0;  // highest value any submission will signal; queue_wait cannot wait past it
};

enum class QueryState : std::uint8_t {
    Empty,     // never written
    Pending,   // written by a submission that has not executed
    Resolved,  // written and executed
};

struct QueryRecord {
    std::vector<std::uint64_t> values;
    std::vector<QueryState> states;
};

struct TimestampWrite {
    ResourceKey pool;
    std::uint32_t index = 0;
};

struct Submission {
    std::vector<ResourceKey> refs;  // one entry per use; sorted and deduplicated before retaining
    std::vector<TimestampWrite> timestamps;
    ResourceKey fence;
    std::uint64_t value = 0;
};

// Maps a descriptor type to the state a resource must be in while a draw or dispatch reads or writes it through it.
ResourceState descriptor_state(DescriptorType type) noexcept {
    switch (type) {
        case DescriptorType::ConstantBuffer:
            return ResourceState::ConstantBuffer;
        case DescriptorType::ShaderResource:
            return ResourceState::ShaderRead;
        case DescriptorType::UnorderedAccess:
            return ResourceState::UnorderedAccess;
    }
    return ResourceState::Common;
}

std::optional<std::size_t> binding_position(const DescriptorSetRecord& set, std::uint32_t binding) {
    for (std::size_t i = 0; i < set.bindings.size(); ++i) {
        if (set.bindings[i].binding == binding) {
            return i;
        }
    }
    return std::nullopt;
}

bool buffer_usage_matches(BufferUsage usage, DescriptorType type) noexcept {
    switch (type) {
        case DescriptorType::ConstantBuffer:
            return has_flags(usage, BufferUsage::Constant);
        case DescriptorType::ShaderResource:
            return has_flags(usage, BufferUsage::ShaderResource);
        case DescriptorType::UnorderedAccess:
            return has_flags(usage, BufferUsage::UnorderedAccess);
    }
    return false;
}

bool texture_usage_matches(TextureUsage usage, DescriptorType type) noexcept {
    switch (type) {
        case DescriptorType::ConstantBuffer:
            return false;
        case DescriptorType::ShaderResource:
            return has_flags(usage, TextureUsage::ShaderResource);
        case DescriptorType::UnorderedAccess:
            return has_flags(usage, TextureUsage::UnorderedAccess);
    }
    return false;
}

bool same_layout(const std::vector<DescriptorBindingDesc>& lhs, const std::vector<DescriptorBindingDesc>& rhs) {
    return lhs == rhs;
}

}  // namespace

struct ValidationDevice::State {
    std::uint32_t device_id = next_device_id();
    CapabilityReport report;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    Pool<BufferTag, BufferRecord> buffers;
    Pool<TextureTag, TextureRecord> textures;
    Pool<DescriptorSetLayoutTag, std::vector<DescriptorBindingDesc>> layouts;
    Pool<DescriptorSetTag, DescriptorSetRecord> sets;
    Pool<PipelineTag, PipelineRecord> pipelines;
    Pool<FenceTag, FenceRecord> fences;
    Pool<QueryPoolTag, QueryRecord> query_pools;
    std::deque<Submission> pending;

    // Whether a key still names an allocated object that has not been destroyed.
    [[nodiscard]] bool live(const ResourceKey& key) const {
        switch (key.kind) {
            case ResourceKind::Buffer:
                return buffers.live_index(key.index, key.generation);
            case ResourceKind::Texture:
                return textures.live_index(key.index, key.generation);
            case ResourceKind::DescriptorSet:
                return sets.live_index(key.index, key.generation);
            case ResourceKind::Pipeline:
                return pipelines.live_index(key.index, key.generation);
            case ResourceKind::Fence:
                return fences.live_index(key.index, key.generation);
            case ResourceKind::QueryPool:
                return query_pools.live_index(key.index, key.generation);
        }
        return false;
    }

    void retain(const ResourceKey& key) {
        switch (key.kind) {
            case ResourceKind::Buffer:
                buffers.retain(key.index);
                break;
            case ResourceKind::Texture:
                textures.retain(key.index);
                break;
            case ResourceKind::DescriptorSet:
                sets.retain(key.index);
                break;
            case ResourceKind::Pipeline:
                pipelines.retain(key.index);
                break;
            case ResourceKind::Fence:
                fences.retain(key.index);
                break;
            case ResourceKind::QueryPool:
                query_pools.retain(key.index);
                break;
        }
    }

    void release(const ResourceKey& key) {
        switch (key.kind) {
            case ResourceKind::Buffer:
                buffers.release(key.index);
                break;
            case ResourceKind::Texture:
                textures.release(key.index);
                break;
            case ResourceKind::DescriptorSet:
                sets.release(key.index);
                break;
            case ResourceKind::Pipeline:
                pipelines.release(key.index);
                break;
            case ResourceKind::Fence:
                fences.release(key.index);
                break;
            case ResourceKind::QueryPool:
                query_pools.release(key.index);
                break;
        }
    }

    [[nodiscard]] std::uint64_t now_ticks() const {
        const auto elapsed = std::chrono::steady_clock::now() - epoch;
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

    // Executes the oldest pending submission: resolves its timestamps, signals its fence, and releases its holds.
    void complete_front() {
        Submission submission = std::move(pending.front());
        pending.pop_front();
        const std::uint64_t now = now_ticks();
        for (const TimestampWrite& write : submission.timestamps) {
            QueryRecord& query = query_pools.at(write.pool.index);
            query.values[write.index] = now;
            query.states[write.index] = QueryState::Resolved;
        }
        FenceRecord& fence = fences.at(submission.fence.index);
        fence.completed = std::max(fence.completed, submission.value);
        for (const ResourceKey& key : submission.refs) {
            release(key);
        }
    }
};

// Records and validates one command list. The tracked state is updated as each command is recorded, so later commands see
// its effects. Fields are public because only ValidationDevice can name this class.
class ValidationDevice::Recorder final : public CommandList {
public:
    Recorder(std::shared_ptr<State> state, QueueType queue) : state_(std::move(state)), queue_(queue) {}

    [[nodiscard]] QueueType queue() const noexcept override {
        return queue_;
    }

    RhiStatus transition(BufferHandle buffer, ResourceState before, ResourceState after) override;
    RhiStatus transition(TextureHandle texture, ResourceState before, ResourceState after) override;
    RhiStatus set_pipeline(PipelineHandle pipeline) override;
    RhiStatus set_descriptor_set(std::uint32_t set_index, DescriptorSetHandle set) override;
    RhiStatus set_vertex_buffer(std::uint32_t slot, BufferHandle buffer) override;
    RhiStatus set_index_buffer(BufferHandle buffer, IndexFormat format) override;
    RhiStatus begin_render_pass(const RenderPassDesc& pass) override;
    RhiStatus end_render_pass() override;
    RhiStatus draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                           std::int32_t base_vertex, std::uint32_t first_instance) override;
    RhiStatus draw_indexed_indirect(BufferHandle args, std::uint64_t args_offset, std::uint32_t draw_count) override;
    RhiStatus draw_indexed_indirect_count(BufferHandle args, std::uint64_t args_offset, BufferHandle count,
                                          std::uint64_t count_offset, std::uint32_t max_draw_count) override;
    RhiStatus dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override;
    RhiStatus dispatch_mesh(std::uint32_t x, std::uint32_t y, std::uint32_t z) override;
    RhiStatus upscale(Upscaler& upscaler, const UpscaleDispatch& dispatch) override;
    RhiStatus build_bottom_level(AccelerationStructureHandle structure, BuildMode mode) override;
    RhiStatus build_top_level(AccelerationStructureHandle structure, BuildMode mode) override;
    RhiStatus set_ray_tracing_pipeline(RayTracingPipelineHandle pipeline) override;
    RhiStatus dispatch_rays(std::uint32_t width, std::uint32_t height, std::uint32_t depth) override;
    RhiStatus copy_buffer(BufferHandle source, std::uint64_t source_offset, BufferHandle destination,
                          std::uint64_t destination_offset, std::uint64_t size) override;
    RhiStatus copy_texture_to_buffer(TextureHandle source, BufferHandle destination, std::uint64_t destination_offset) override;
    RhiStatus copy_buffer_to_texture(BufferHandle source, std::uint64_t source_offset, TextureHandle destination) override;
    RhiStatus write_timestamp(QueryPoolHandle pool, std::uint32_t index) override;
    RhiStatus close() override;

    enum class ListState : std::uint8_t { Recording, Closed, Submitted };

    std::shared_ptr<State> state_;
    QueueType queue_;
    ListState list_state_ = ListState::Recording;
    std::vector<ResourceKey> refs_;
    std::vector<TimestampWrite> timestamps_;

    std::optional<PipelineHandle> pipeline_;
    PipelineKind pipeline_kind_ = PipelineKind::Graphics;
    std::array<DescriptorSetHandle, kMaxDescriptorSetsPerPipeline> sets_{};
    std::uint32_t bound_set_mask_ = 0;
    std::array<std::optional<BufferHandle>, kMaxVertexBufferSlots> vertex_buffers_{};
    std::optional<BufferHandle> index_buffer_;
    IndexFormat index_format_ = IndexFormat::Uint32;
    bool pass_active_ = false;
    std::vector<Format> pass_colors_;
    Format pass_depth_ = Format::Unknown;

private:
    RhiStatus require_recording(std::string_view op) const;
    RhiStatus check_compute_state(std::string_view op);
    RhiStatus check_graphics_state(std::string_view op, PipelineKind expected);
    RhiStatus check_vertex_and_index_state(std::string_view op);

    RhiStatus check_bound_sets(const PipelineRecord& pipeline, std::string_view op);
    void use(const ResourceKey& key) {
        refs_.push_back(key);
    }
};

// ---- ValidationDevice ------------------------------------------------------------------------------------------------

namespace {

// The reference device keeps no acceleration structures or shader tables. A feature the device was not asked to simulate is
// FeatureDisabled, as for every other feature; a simulated one is Unsupported, because the contract is not implemented here.
RhiError ray_tracing_error(const CapabilityReport& report, std::string_view operation) {
    if (!report.enabled(Feature::RayTracing)) {
        return RhiError{RhiErrorCode::FeatureDisabled, std::string(operation) + ": the RayTracing feature is not enabled"};
    }
    return RhiError{RhiErrorCode::Unsupported,
                    std::string(operation) + ": the reference device has no acceleration structures; run ray tracing on a GPU backend"};
}

}  // namespace

ValidationDevice::ValidationDevice(const ValidationDeviceDesc& desc) : state_(std::make_shared<State>()) {
    CapabilityReport& report = state_->report;
    report.backend = Backend::Validation;
    report.adapter_name = desc.adapter_name;
    report.timestamp_frequency_hz = kTimestampFrequencyHz;
    for (const Feature feature : kFeatureList) {
        FeatureState feature_state = FeatureState::Unsupported;
        if (has_flags(desc.simulated_hardware, feature)) {
            feature_state = has_flags(desc.disabled_features, feature) ? FeatureState::DisabledForTest : FeatureState::Supported;
        }
        report.features[feature_index(feature)] = feature_state;
    }
}

Backend ValidationDevice::backend() const noexcept {
    return Backend::Validation;
}

const CapabilityReport& ValidationDevice::capabilities() const noexcept {
    return state_->report;
}

RhiResult<BufferHandle> ValidationDevice::create_buffer(const BufferDesc& desc) {
    if (desc.size_bytes == 0) {
        return fail<BufferHandle>(RhiErrorCode::InvalidDescriptor, "create_buffer: size must be greater than zero");
    }
    if (desc.usage == BufferUsage::None) {
        return fail<BufferHandle>(RhiErrorCode::InvalidDescriptor, "create_buffer: usage must name at least one use");
    }
    if (has_flags(desc.usage, BufferUsage::CpuRead) && !has_flags(desc.usage, BufferUsage::CopyDestination)) {
        return fail<BufferHandle>(RhiErrorCode::InvalidDescriptor, "create_buffer: CpuRead needs CopyDestination usage");
    }
    if (has_flags(desc.usage, BufferUsage::CpuWrite) && !has_flags(desc.usage, BufferUsage::CopySource)) {
        return fail<BufferHandle>(RhiErrorCode::InvalidDescriptor, "create_buffer: CpuWrite needs CopySource usage");
    }
    if (has_flags(desc.usage, BufferUsage::CpuRead) && has_flags(desc.usage, BufferUsage::CpuWrite)) {
        return fail<BufferHandle>(RhiErrorCode::InvalidDescriptor, "create_buffer: CpuRead and CpuWrite cannot be combined");
    }
    // Readback memory starts in CopyDestination and upload memory in CopySource, the only states each allows.
    ResourceState initial = ResourceState::Common;
    if (has_flags(desc.usage, BufferUsage::CpuRead)) {
        initial = ResourceState::CopyDestination;
    } else if (has_flags(desc.usage, BufferUsage::CpuWrite)) {
        initial = ResourceState::CopySource;
    }
    return core::success(state_->buffers.add(BufferRecord{desc, initial}, state_->device_id));
}

RhiResult<TextureHandle> ValidationDevice::create_texture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return fail<TextureHandle>(RhiErrorCode::InvalidDescriptor, "create_texture: width and height must be greater than zero");
    }
    if (desc.format == Format::Unknown) {
        return fail<TextureHandle>(RhiErrorCode::InvalidDescriptor, "create_texture: a format is required");
    }
    if (desc.usage == TextureUsage::None) {
        return fail<TextureHandle>(RhiErrorCode::InvalidDescriptor, "create_texture: usage must name at least one use");
    }
    std::uint32_t max_levels = 1;
    for (std::uint32_t dim = std::max(desc.width, desc.height); dim > 1; dim >>= 1) {
        ++max_levels;
    }
    if (desc.mip_levels == 0 || desc.mip_levels > max_levels) {
        return fail<TextureHandle>(RhiErrorCode::OutOfRange, "create_texture: mip_levels must be between 1 and the full chain");
    }
    if (has_flags(desc.usage, TextureUsage::DepthStencil) && !is_depth_format(desc.format)) {
        return fail<TextureHandle>(RhiErrorCode::InvalidDescriptor, "create_texture: depth-stencil usage needs a depth format");
    }
    if (has_flags(desc.usage, TextureUsage::RenderTarget) && is_depth_format(desc.format)) {
        return fail<TextureHandle>(RhiErrorCode::InvalidDescriptor, "create_texture: render-target usage needs a color format");
    }
    return core::success(state_->textures.add(TextureRecord{desc, ResourceState::Common}, state_->device_id));
}

RhiResult<DescriptorSetLayoutHandle> ValidationDevice::create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc) {
    for (std::size_t i = 0; i < desc.bindings.size(); ++i) {
        const DescriptorBindingDesc& binding = desc.bindings[i];
        if (binding.count == 0) {
            return fail<DescriptorSetLayoutHandle>(RhiErrorCode::InvalidDescriptor, "descriptor binding count must be at least one");
        }
        if (binding.visibility == ShaderStage::None) {
            return fail<DescriptorSetLayoutHandle>(RhiErrorCode::InvalidDescriptor, "descriptor binding needs a shader stage");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (desc.bindings[j].binding == binding.binding) {
                return fail<DescriptorSetLayoutHandle>(RhiErrorCode::InvalidDescriptor, "descriptor layout repeats a binding index");
            }
        }
    }
    return core::success(state_->layouts.add(desc.bindings, state_->device_id));
}

RhiResult<DescriptorSetHandle> ValidationDevice::create_descriptor_set(DescriptorSetLayoutHandle layout) {
    const std::vector<DescriptorBindingDesc>* bindings = state_->layouts.find(layout, state_->device_id);
    if (bindings == nullptr) {
        return fail<DescriptorSetHandle>(RhiErrorCode::InvalidHandle, "create_descriptor_set: layout handle is invalid or destroyed");
    }
    DescriptorSetRecord record;
    record.bindings = *bindings;
    std::uint32_t total = 0;
    for (const DescriptorBindingDesc& binding : record.bindings) {
        record.first_entry.push_back(total);
        total += binding.count;
    }
    record.entries.resize(total);
    for (std::size_t i = 0; i < record.bindings.size(); ++i) {
        for (std::uint32_t k = 0; k < record.bindings[i].count; ++k) {
            record.entries[record.first_entry[i] + k].type = record.bindings[i].type;
        }
    }
    return core::success(state_->sets.add(std::move(record), state_->device_id));
}

RhiResult<PipelineHandle> ValidationDevice::create_pipeline(const PipelineDesc& desc) {
    if (desc.set_layouts.size() > kMaxDescriptorSetsPerPipeline) {
        return fail<PipelineHandle>(RhiErrorCode::OutOfRange, "create_pipeline: too many descriptor set layouts");
    }
    if (desc.color_formats.size() > kMaxColorTargets) {
        return fail<PipelineHandle>(RhiErrorCode::OutOfRange, "create_pipeline: too many color targets");
    }
    if (desc.kind == PipelineKind::Mesh && !state_->report.enabled(Feature::MeshShaders)) {
        return fail<PipelineHandle>(RhiErrorCode::FeatureDisabled, "create_pipeline: mesh pipelines need the MeshShaders feature");
    }
    for (const Format format : desc.color_formats) {
        if (format == Format::Unknown || is_depth_format(format)) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: color targets need a color format");
        }
    }
    if (desc.depth_format != Format::Unknown && !is_depth_format(desc.depth_format)) {
        return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: depth target needs a depth format");
    }
    if (const RhiStatus layout = check_vertex_layout(desc); !layout.ok()) {
        return core::failure(layout.error());
    }
    if (const RhiStatus blend = check_blend_state(desc); !blend.ok()) {
        return core::failure(blend.error());
    }

    bool has_vertex = false;
    bool has_pixel = false;
    bool has_compute = false;
    bool has_mesh = false;
    bool has_amplification = false;
    for (const ShaderBinary& shader : desc.shaders) {
        if (shader.entry_point.empty() || shader.bytecode.empty()) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: every shader needs an entry point and bytecode");
        }
        bool* seen = nullptr;
        switch (shader.stage) {
            case ShaderStage::Vertex:
                seen = &has_vertex;
                break;
            case ShaderStage::Pixel:
                seen = &has_pixel;
                break;
            case ShaderStage::Compute:
                seen = &has_compute;
                break;
            case ShaderStage::Mesh:
                seen = &has_mesh;
                break;
            case ShaderStage::Amplification:
                seen = &has_amplification;
                break;
            case ShaderStage::None:
            case ShaderStage::RayTracing:
                return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: shader has no pipeline stage");
        }
        if (*seen) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: a stage appears twice");
        }
        *seen = true;
    }
    bool stages_match = false;
    switch (desc.kind) {
        case PipelineKind::Graphics:
            stages_match = has_vertex && has_pixel && !has_compute && !has_mesh && !has_amplification;
            break;
        case PipelineKind::Compute:
            stages_match = has_compute && !has_vertex && !has_pixel && !has_mesh && !has_amplification;
            break;
        case PipelineKind::Mesh:
            // The amplification stage is optional; the mesh and pixel stages are not.
            stages_match = has_mesh && has_pixel && !has_vertex && !has_compute;
            break;
    }
    if (!stages_match) {
        return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: the shader stages do not match the pipeline kind");
    }

    PipelineRecord record;
    record.kind = desc.kind;
    record.per_instance = !desc.instance_attributes.empty();
    record.color_formats = desc.color_formats;
    record.depth_format = desc.depth_format;
    for (const DescriptorSetLayoutHandle& layout : desc.set_layouts) {
        const std::vector<DescriptorBindingDesc>* bindings = state_->layouts.find(layout, state_->device_id);
        if (bindings == nullptr) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidHandle, "create_pipeline: a set layout handle is invalid or destroyed");
        }
        record.layouts.push_back(*bindings);
    }
    return core::success(state_->pipelines.add(std::move(record), state_->device_id));
}

RhiResult<FenceHandle> ValidationDevice::create_fence(std::uint64_t initial_value) {
    return core::success(state_->fences.add(FenceRecord{initial_value}, state_->device_id));
}

RhiResult<QueryPoolHandle> ValidationDevice::create_query_pool(std::uint32_t count) {
    if (count == 0 || count > kMaxQueryCount) {
        return fail<QueryPoolHandle>(RhiErrorCode::OutOfRange, "create_query_pool: count must be between 1 and kMaxQueryCount");
    }
    QueryRecord record;
    record.values.assign(count, 0);
    record.states.assign(count, QueryState::Empty);
    return core::success(state_->query_pools.add(std::move(record), state_->device_id));
}

RhiResult<AccelerationStructureHandle> ValidationDevice::create_bottom_level(const BottomLevelDesc&) {
    return core::failure(ray_tracing_error(state_->report, "create_bottom_level"));
}

RhiResult<AccelerationStructureHandle> ValidationDevice::create_top_level(const TopLevelDesc&) {
    return core::failure(ray_tracing_error(state_->report, "create_top_level"));
}

RhiStatus ValidationDevice::write_top_level(AccelerationStructureHandle, const std::vector<TopLevelInstance>&) {
    return core::failure(ray_tracing_error(state_->report, "write_top_level"));
}

RhiResult<RayTracingPipelineHandle> ValidationDevice::create_ray_tracing_pipeline(const RayTracingPipelineDesc&) {
    return core::failure(ray_tracing_error(state_->report, "create_ray_tracing_pipeline"));
}

RhiResult<std::unique_ptr<CommandList>> ValidationDevice::create_command_list(QueueType queue) {
    std::unique_ptr<CommandList> list = std::make_unique<Recorder>(state_, queue);
    return core::success(std::move(list));
}

RhiStatus ValidationDevice::write_buffer(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index, BufferHandle buffer) {
    DescriptorSetRecord* record = state_->sets.find(set, state_->device_id);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_buffer: descriptor set handle is invalid or destroyed");
    }
    const BufferRecord* target = state_->buffers.find(buffer, state_->device_id);
    if (target == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_buffer: buffer handle is invalid or destroyed");
    }
    const std::optional<std::size_t> position = binding_position(*record, binding);
    if (!position) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_buffer: the set has no such binding");
    }
    const DescriptorBindingDesc& desc = record->bindings[*position];
    if (array_index >= desc.count) {
        return fail(RhiErrorCode::OutOfRange, "write_buffer: array index is outside the binding");
    }
    if (!buffer_usage_matches(target->desc.usage, desc.type)) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_buffer: buffer usage does not match the binding type");
    }
    DescriptorEntry& entry = record->entries[record->first_entry[*position] + array_index];
    entry.written = true;
    entry.key = key_of(ResourceKind::Buffer, buffer);
    return core::success();
}

RhiStatus ValidationDevice::write_texture(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index, TextureHandle texture) {
    DescriptorSetRecord* record = state_->sets.find(set, state_->device_id);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_texture: descriptor set handle is invalid or destroyed");
    }
    const TextureRecord* target = state_->textures.find(texture, state_->device_id);
    if (target == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_texture: texture handle is invalid or destroyed");
    }
    const std::optional<std::size_t> position = binding_position(*record, binding);
    if (!position) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_texture: the set has no such binding");
    }
    const DescriptorBindingDesc& desc = record->bindings[*position];
    if (array_index >= desc.count) {
        return fail(RhiErrorCode::OutOfRange, "write_texture: array index is outside the binding");
    }
    if (!texture_usage_matches(target->desc.usage, desc.type)) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_texture: texture usage does not match the binding type");
    }
    DescriptorEntry& entry = record->entries[record->first_entry[*position] + array_index];
    entry.written = true;
    entry.key = key_of(ResourceKind::Texture, texture);
    return core::success();
}

// Destroys one object. The handle is invalid from here on; the slot is kept while a submission holds it.
template <typename Tag, typename Record>
RhiStatus destroy_in(Pool<Tag, Record>& pool, Handle<Tag> handle, std::uint32_t device_id, std::string_view what) {
    if (!pool.live(handle, device_id)) {
        return fail(RhiErrorCode::InvalidHandle, std::string("destroy: ") + std::string(what) + " handle is invalid or already destroyed");
    }
    pool.destroy(handle.index);
    return core::success();
}

RhiStatus ValidationDevice::destroy(BufferHandle buffer) {
    return destroy_in(state_->buffers, buffer, state_->device_id, "buffer");
}
RhiStatus ValidationDevice::destroy(TextureHandle texture) {
    return destroy_in(state_->textures, texture, state_->device_id, "texture");
}
RhiStatus ValidationDevice::destroy(DescriptorSetLayoutHandle layout) {
    return destroy_in(state_->layouts, layout, state_->device_id, "descriptor set layout");
}
RhiStatus ValidationDevice::destroy(DescriptorSetHandle set) {
    return destroy_in(state_->sets, set, state_->device_id, "descriptor set");
}
RhiStatus ValidationDevice::destroy(PipelineHandle pipeline) {
    return destroy_in(state_->pipelines, pipeline, state_->device_id, "pipeline");
}
RhiStatus ValidationDevice::destroy(FenceHandle fence) {
    return destroy_in(state_->fences, fence, state_->device_id, "fence");
}
RhiStatus ValidationDevice::write_acceleration_structure(DescriptorSetHandle, std::uint32_t, std::uint32_t, AccelerationStructureHandle) {
    return core::failure(ray_tracing_error(state_->report, "write_acceleration_structure"));
}

RhiStatus ValidationDevice::destroy(AccelerationStructureHandle) {
    return core::failure(ray_tracing_error(state_->report, "destroy"));
}

RhiStatus ValidationDevice::destroy(RayTracingPipelineHandle) {
    return core::failure(ray_tracing_error(state_->report, "destroy"));
}

RhiStatus ValidationDevice::destroy(QueryPoolHandle pool) {
    return destroy_in(state_->query_pools, pool, state_->device_id, "query pool");
}

RhiStatus ValidationDevice::submit(QueueType queue, const std::vector<CommandList*>& lists, FenceHandle signal_fence,
                                   std::uint64_t signal_value) {
    if (!state_->fences.live(signal_fence, state_->device_id)) {
        return fail(RhiErrorCode::InvalidHandle, "submit: signal fence handle is invalid or destroyed");
    }
    // Validate every list before any changes, so a rejected submission leaves all lists and objects as they were.
    std::vector<Recorder*> recorders;
    for (CommandList* list : lists) {
        auto* recorder = dynamic_cast<Recorder*>(list);
        if (recorder == nullptr || recorder->state_ != state_) {
            return fail(RhiErrorCode::InvalidHandle, "submit: command list belongs to another device");
        }
        if (recorder->list_state_ != Recorder::ListState::Closed) {
            return fail(RhiErrorCode::InvalidState, "submit: command list must be closed and not yet submitted");
        }
        if (recorder->queue_ != queue) {
            return fail(RhiErrorCode::InvalidState, "submit: command list was recorded for another queue");
        }
        if (std::find(recorders.begin(), recorders.end(), recorder) != recorders.end()) {
            return fail(RhiErrorCode::InvalidState, "submit: the same command list appears twice");
        }
        for (const ResourceKey& key : recorder->refs_) {
            if (!state_->live(key)) {
                return fail(RhiErrorCode::InvalidHandle, "submit: a command list uses an object destroyed after it was recorded");
            }
        }
        recorders.push_back(recorder);
    }

    Submission submission;
    submission.fence = key_of(ResourceKind::Fence, signal_fence);
    submission.value = signal_value;
    for (Recorder* recorder : recorders) {
        submission.refs.insert(submission.refs.end(), recorder->refs_.begin(), recorder->refs_.end());
        submission.timestamps.insert(submission.timestamps.end(), recorder->timestamps_.begin(), recorder->timestamps_.end());
        recorder->list_state_ = Recorder::ListState::Submitted;
    }
    submission.refs.push_back(submission.fence);

    // Each object is held once per submission, however many commands used it. Holds are taken after validation.
    std::sort(submission.refs.begin(), submission.refs.end());
    submission.refs.erase(std::unique(submission.refs.begin(), submission.refs.end()), submission.refs.end());
    for (const ResourceKey& key : submission.refs) {
        state_->retain(key);
    }
    for (const TimestampWrite& write : submission.timestamps) {
        state_->query_pools.at(write.pool.index).states[write.index] = QueryState::Pending;
    }
    FenceRecord& signaled = state_->fences.at(submission.fence.index);
    signaled.signaled = std::max(signaled.signaled, submission.value);
    state_->pending.push_back(std::move(submission));
    return core::success();
}

RhiResult<std::uint64_t> ValidationDevice::fence_value(FenceHandle fence) {
    const FenceRecord* record = state_->fences.find(fence, state_->device_id);
    if (record == nullptr) {
        return fail<std::uint64_t>(RhiErrorCode::InvalidHandle, "fence_value: fence handle is invalid or destroyed");
    }
    return core::success(record->completed);
}

RhiStatus ValidationDevice::wait_fence(FenceHandle fence, std::uint64_t value) {
    if (!state_->fences.live(fence, state_->device_id)) {
        return fail(RhiErrorCode::InvalidHandle, "wait_fence: fence handle is invalid or destroyed");
    }
    // Runs submissions in order until the fence reaches the value. Only the fence's own record is read afterwards.
    const ResourceKey key = key_of(ResourceKind::Fence, fence);
    while (state_->fences.at(key.index).completed < value && !state_->pending.empty()) {
        state_->complete_front();
    }
    if (state_->fences.at(key.index).completed < value) {
        return fail(RhiErrorCode::InvalidState, "wait_fence: no pending submission reaches the requested fence value");
    }
    return core::success();
}

RhiStatus ValidationDevice::wait_idle() {
    while (!state_->pending.empty()) {
        state_->complete_front();
    }
    return core::success();
}

RhiResult<std::uint64_t> ValidationDevice::read_timestamp(QueryPoolHandle pool, std::uint32_t index) {
    const QueryRecord* query = state_->query_pools.find(pool, state_->device_id);
    if (query == nullptr) {
        return fail<std::uint64_t>(RhiErrorCode::InvalidHandle, "read_timestamp: query pool handle is invalid or destroyed");
    }
    if (index >= query->values.size()) {
        return fail<std::uint64_t>(RhiErrorCode::OutOfRange, "read_timestamp: index is outside the query pool");
    }
    if (query->states[index] != QueryState::Resolved) {
        return fail<std::uint64_t>(RhiErrorCode::NotReady, "read_timestamp: the submission that wrote this timestamp has not completed");
    }
    return core::success(query->values[index]);
}

RhiStatus ValidationDevice::read_buffer(BufferHandle buffer, std::uint64_t offset, std::uint64_t size, void* destination) {
    (void)buffer;
    (void)offset;
    (void)size;
    (void)destination;
    return fail(RhiErrorCode::Unsupported, "read_buffer: the reference device keeps no buffer contents; read back on a GPU backend");
}

RhiStatus ValidationDevice::upload_buffer(BufferHandle buffer, std::uint64_t offset, std::uint64_t size, const void* source) {
    (void)source;
    const BufferRecord* record = state_->buffers.find(buffer, state_->device_id);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "upload_buffer: buffer handle is invalid or destroyed");
    }
    if (!has_flags(record->desc.usage, BufferUsage::CpuWrite)) {
        return fail(RhiErrorCode::InvalidDescriptor, "upload_buffer: the buffer was not created with CpuWrite usage");
    }
    if (size == 0 || offset > record->desc.size_bytes || size > record->desc.size_bytes - offset) {
        return fail(RhiErrorCode::OutOfRange, "upload_buffer: range is outside the buffer");
    }
    return fail(RhiErrorCode::Unsupported, "upload_buffer: the reference device keeps no buffer contents; upload on a GPU backend");
}

RhiStatus ValidationDevice::queue_wait(QueueType queue, FenceHandle fence, std::uint64_t value) {
    (void)queue;
    if (!state_->fences.live(fence, state_->device_id)) {
        return fail(RhiErrorCode::InvalidHandle, "queue_wait: fence handle is invalid or destroyed");
    }
    if (value > state_->fences.at(fence.index).signaled) {
        return fail(RhiErrorCode::InvalidState, "queue_wait: no submission has signaled that fence value yet");
    }
    return core::success();
}

RhiResult<ResourceStats> ValidationDevice::resource_stats() {
    ResourceStats stats;
    stats.buffers = static_cast<std::uint32_t>(state_->buffers.allocated());
    stats.textures = static_cast<std::uint32_t>(state_->textures.allocated());
    stats.descriptor_sets = static_cast<std::uint32_t>(state_->sets.allocated());
    // The reference device keeps no shader-visible heap; a submission holds its objects until it executes.
    stats.retained_holds = static_cast<std::uint32_t>(state_->pending.size());
    return core::success(stats);
}

RhiResult<std::unique_ptr<Swapchain>> ValidationDevice::create_swapchain(const SwapchainDesc& desc) {
    (void)desc;
    return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::Unsupported,
                                            "create_swapchain: the reference device has no presentation surface");
}

RhiResult<std::unique_ptr<Upscaler>> ValidationDevice::create_upscaler(const UpscalerDesc& desc) {
    (void)desc;
    return fail<std::unique_ptr<Upscaler>>(RhiErrorCode::Unsupported, "create_upscaler: the reference device runs no FSR 2 upscale");
}

// The reference device returns every error from its own calls and keeps no layer messages.
RhiStatus ValidationDevice::check_debug_messages() {
    return core::success();
}

std::size_t ValidationDevice::allocated_object_count() const noexcept {
    return state_->buffers.allocated() + state_->textures.allocated() + state_->layouts.allocated() + state_->sets.allocated() +
           state_->pipelines.allocated() + state_->fences.allocated() + state_->query_pools.allocated();
}

std::size_t ValidationDevice::pending_submission_count() const noexcept {
    return state_->pending.size();
}

// ---- Recorder ----------------------------------------------------------------------------------------------------------

RhiStatus ValidationDevice::Recorder::require_recording(std::string_view op) const {
    if (list_state_ != ListState::Recording) {
        return fail(RhiErrorCode::InvalidState, std::string(op) + ": the command list is closed or already submitted");
    }
    return core::success();
}

RhiStatus ValidationDevice::Recorder::transition(BufferHandle buffer, ResourceState before, ResourceState after) {
    if (const RhiStatus open = require_recording("transition"); !open.ok()) {
        return open;
    }
    BufferRecord* record = state_->buffers.find(buffer, state_->device_id);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "transition: buffer handle is invalid or destroyed");
    }
    if (record->state != before) {
        return fail(RhiErrorCode::StateMismatch, "transition: buffer is not in the state the caller named as 'before'");
    }
    if (!buffer_state_allowed(record->desc.usage, after)) {
        return fail(RhiErrorCode::InvalidDescriptor, "transition: the buffer usage does not allow the target state");
    }
    record->state = after;
    use(key_of(ResourceKind::Buffer, buffer));
    return core::success();
}

RhiStatus ValidationDevice::Recorder::transition(TextureHandle texture, ResourceState before, ResourceState after) {
    if (const RhiStatus open = require_recording("transition"); !open.ok()) {
        return open;
    }
    TextureRecord* record = state_->textures.find(texture, state_->device_id);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "transition: texture handle is invalid or destroyed");
    }
    if (record->state != before) {
        return fail(RhiErrorCode::StateMismatch, "transition: texture is not in the state the caller named as 'before'");
    }
    if (!texture_state_allowed(record->desc.usage, after)) {
        return fail(RhiErrorCode::InvalidDescriptor, "transition: the texture usage does not allow the target state");
    }
    record->state = after;
    use(key_of(ResourceKind::Texture, texture));
    return core::success();
}

RhiStatus ValidationDevice::Recorder::set_pipeline(PipelineHandle pipeline) {
    if (const RhiStatus open = require_recording("set_pipeline"); !open.ok()) {
        return open;
    }
    const PipelineRecord* record = state_->pipelines.find(pipeline, state_->device_id);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_pipeline: pipeline handle is invalid or destroyed");
    }
    if (record->kind == PipelineKind::Compute ? queue_ == QueueType::Copy : queue_ != QueueType::Graphics) {
        return fail(RhiErrorCode::InvalidState, "set_pipeline: this pipeline kind cannot run on this queue");
    }
    pipeline_ = pipeline;
    pipeline_kind_ = record->kind;
    // A new pipeline has its own layout, so earlier descriptor-set bindings no longer apply.
    bound_set_mask_ = 0;
    sets_.fill(DescriptorSetHandle{});
    use(key_of(ResourceKind::Pipeline, pipeline));
    return core::success();
}

RhiStatus ValidationDevice::Recorder::set_descriptor_set(std::uint32_t set_index, DescriptorSetHandle set) {
    if (const RhiStatus open = require_recording("set_descriptor_set"); !open.ok()) {
        return open;
    }
    if (!pipeline_) {
        return fail(RhiErrorCode::InvalidState, "set_descriptor_set: bind a pipeline first");
    }
    const PipelineRecord* pipeline = state_->pipelines.find(*pipeline_, state_->device_id);
    if (pipeline == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_descriptor_set: the bound pipeline was destroyed");
    }
    if (set_index >= pipeline->layouts.size()) {
        return fail(RhiErrorCode::OutOfRange, "set_descriptor_set: the pipeline has no descriptor set at this index");
    }
    const DescriptorSetRecord* record = state_->sets.find(set, state_->device_id);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_descriptor_set: descriptor set handle is invalid or destroyed");
    }
    if (!same_layout(record->bindings, pipeline->layouts[set_index])) {
        return fail(RhiErrorCode::InvalidDescriptor, "set_descriptor_set: the set's layout does not match the pipeline layout");
    }
    sets_[set_index] = set;
    bound_set_mask_ |= 1u << set_index;
    use(key_of(ResourceKind::DescriptorSet, set));
    for (const DescriptorEntry& entry : record->entries) {
        if (entry.written) {
            use(entry.key);
        }
    }
    return core::success();
}

RhiStatus ValidationDevice::Recorder::set_vertex_buffer(std::uint32_t slot, BufferHandle buffer) {
    if (const RhiStatus open = require_recording("set_vertex_buffer"); !open.ok()) {
        return open;
    }
    if (slot >= kMaxVertexBufferSlots) {
        return fail(RhiErrorCode::OutOfRange, "set_vertex_buffer: slot is outside the vertex buffer table");
    }
    const BufferRecord* record = state_->buffers.find(buffer, state_->device_id);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_vertex_buffer: buffer handle is invalid or destroyed");
    }
    if (!has_flags(record->desc.usage, BufferUsage::Vertex)) {
        return fail(RhiErrorCode::InvalidDescriptor, "set_vertex_buffer: buffer was not created with Vertex usage");
    }
    vertex_buffers_[slot] = buffer;
    use(key_of(ResourceKind::Buffer, buffer));
    return core::success();
}

RhiStatus ValidationDevice::Recorder::set_index_buffer(BufferHandle buffer, IndexFormat format) {
    if (const RhiStatus open = require_recording("set_index_buffer"); !open.ok()) {
        return open;
    }
    const BufferRecord* record = state_->buffers.find(buffer, state_->device_id);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_index_buffer: buffer handle is invalid or destroyed");
    }
    if (!has_flags(record->desc.usage, BufferUsage::Index)) {
        return fail(RhiErrorCode::InvalidDescriptor, "set_index_buffer: buffer was not created with Index usage");
    }
    if (record->desc.size_bytes % index_stride_bytes(format) != 0) {
        return fail(RhiErrorCode::InvalidDescriptor, "set_index_buffer: buffer size is not a multiple of the index size");
    }
    index_buffer_ = buffer;
    index_format_ = format;
    use(key_of(ResourceKind::Buffer, buffer));
    return core::success();
}

RhiStatus ValidationDevice::Recorder::begin_render_pass(const RenderPassDesc& pass) {
    if (const RhiStatus open = require_recording("begin_render_pass"); !open.ok()) {
        return open;
    }
    if (queue_ != QueueType::Graphics) {
        return fail(RhiErrorCode::InvalidState, "begin_render_pass: render passes need the graphics queue");
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "begin_render_pass: a render pass is already active");
    }
    if (pass.color_targets.empty() && !pass.depth_target) {
        return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: the pass needs at least one target");
    }
    if (pass.color_targets.size() > kMaxColorTargets) {
        return fail(RhiErrorCode::OutOfRange, "begin_render_pass: too many color targets");
    }
    if (pass.clear_depth && !pass.depth_target) {
        return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: clear_depth needs a depth target");
    }

    std::vector<Format> colors;
    std::vector<ResourceKey> keys;
    for (const TextureHandle& target : pass.color_targets) {
        const TextureRecord* record = state_->textures.find(target, state_->device_id);
        if (record == nullptr) {
            return fail(RhiErrorCode::InvalidHandle, "begin_render_pass: render target handle is invalid or destroyed");
        }
        if (!has_flags(record->desc.usage, TextureUsage::RenderTarget)) {
            return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: texture was not created with RenderTarget usage");
        }
        if (record->state != ResourceState::RenderTarget) {
            return fail(RhiErrorCode::StateMismatch, "begin_render_pass: render target must be in the RenderTarget state");
        }
        colors.push_back(record->desc.format);
        keys.push_back(key_of(ResourceKind::Texture, target));
    }
    Format depth = Format::Unknown;
    if (pass.depth_target) {
        const TextureRecord* record = state_->textures.find(*pass.depth_target, state_->device_id);
        if (record == nullptr) {
            return fail(RhiErrorCode::InvalidHandle, "begin_render_pass: depth target handle is invalid or destroyed");
        }
        if (!has_flags(record->desc.usage, TextureUsage::DepthStencil)) {
            return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: texture was not created with DepthStencil usage");
        }
        if (record->state != ResourceState::DepthWrite) {
            return fail(RhiErrorCode::StateMismatch, "begin_render_pass: depth target must be in the DepthWrite state");
        }
        depth = record->desc.format;
        keys.push_back(key_of(ResourceKind::Texture, *pass.depth_target));
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        for (std::size_t j = i + 1; j < keys.size(); ++j) {
            if (keys[i] == keys[j]) {
                return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: the pass names the same texture twice");
            }
        }
    }
    for (const ResourceKey& key : keys) {
        use(key);
    }
    pass_active_ = true;
    pass_colors_ = std::move(colors);
    pass_depth_ = depth;
    return core::success();
}

RhiStatus ValidationDevice::Recorder::end_render_pass() {
    if (const RhiStatus open = require_recording("end_render_pass"); !open.ok()) {
        return open;
    }
    if (!pass_active_) {
        return fail(RhiErrorCode::InvalidState, "end_render_pass: no render pass is active");
    }
    pass_active_ = false;
    pass_colors_.clear();
    pass_depth_ = Format::Unknown;
    return core::success();
}

// Checks the graphics state a draw needs: the queue, an active pass, a pipeline of the expected kind whose target formats
// match the pass, the descriptor sets it reads, and the state of every bound buffer.
RhiStatus ValidationDevice::Recorder::check_graphics_state(std::string_view op, PipelineKind expected) {
    const std::string name(op);
    if (queue_ != QueueType::Graphics) {
        return fail(RhiErrorCode::InvalidState, name + ": draws need the graphics queue");
    }
    if (!pass_active_) {
        return fail(RhiErrorCode::InvalidState, name + ": needs an active render pass");
    }
    if (!pipeline_ || pipeline_kind_ != expected) {
        return fail(RhiErrorCode::InvalidState, name + ": needs a bound pipeline of the matching kind");
    }
    const PipelineRecord* pipeline = state_->pipelines.find(*pipeline_, state_->device_id);
    if (pipeline == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, name + ": the bound pipeline was destroyed");
    }
    if (pipeline->color_formats != pass_colors_ || pipeline->depth_format != pass_depth_) {
        return fail(RhiErrorCode::InvalidDescriptor, name + ": the pipeline's target formats do not match the render pass");
    }
    return check_bound_sets(*pipeline, op);
}

RhiStatus ValidationDevice::Recorder::check_compute_state(std::string_view op) {
    const std::string name(op);
    if (queue_ == QueueType::Copy) {
        return fail(RhiErrorCode::InvalidState, name + ": compute work needs the graphics or compute queue");
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, name + ": compute work cannot run inside a render pass");
    }
    if (!pipeline_ || pipeline_kind_ != PipelineKind::Compute) {
        return fail(RhiErrorCode::InvalidState, name + ": needs a bound compute pipeline");
    }
    const PipelineRecord* pipeline = state_->pipelines.find(*pipeline_, state_->device_id);
    if (pipeline == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, name + ": the bound pipeline was destroyed");
    }
    return check_bound_sets(*pipeline, op);
}

// Every layout of the pipeline must be bound, every binding written, and every resource in the state its binding needs.
RhiStatus ValidationDevice::Recorder::check_bound_sets(const PipelineRecord& pipeline, std::string_view op) {
    const std::string name(op);
    const std::size_t count = pipeline.layouts.size();
    const std::uint32_t wanted = count == 0 ? 0u : static_cast<std::uint32_t>((1ull << count) - 1ull);
    if ((bound_set_mask_ & wanted) != wanted) {
        return fail(RhiErrorCode::InvalidState, name + ": a descriptor set required by the pipeline is not bound");
    }
    for (std::size_t set_index = 0; set_index < count; ++set_index) {
        const DescriptorSetRecord* set = state_->sets.find(sets_[set_index], state_->device_id);
        if (set == nullptr) {
            return fail(RhiErrorCode::InvalidHandle, name + ": a bound descriptor set was destroyed");
        }
        for (const DescriptorEntry& entry : set->entries) {
            if (!entry.written) {
                return fail(RhiErrorCode::InvalidState, name + ": a bound descriptor was never written");
            }
            if (!state_->live(entry.key)) {
                return fail(RhiErrorCode::InvalidHandle, name + ": a bound descriptor refers to a destroyed resource");
            }
            const ResourceState actual = entry.key.kind == ResourceKind::Buffer ? state_->buffers.at(entry.key.index).state
                                                                              : state_->textures.at(entry.key.index).state;
            if (actual != descriptor_state(entry.type)) {
                return fail(RhiErrorCode::StateMismatch, name + ": a bound resource is not in the state its descriptor needs");
            }
        }
    }
    return core::success();
}

RhiStatus ValidationDevice::Recorder::draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                                                   std::int32_t /*base_vertex*/, std::uint32_t /*first_instance*/) {
    if (const RhiStatus open = require_recording("draw_indexed"); !open.ok()) {
        return open;
    }
    if (index_count == 0 || instance_count == 0) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed: index and instance counts must be greater than zero");
    }
    if (!index_buffer_) {
        return fail(RhiErrorCode::InvalidState, "draw_indexed: an index buffer must be bound");
    }
    const BufferRecord* indices = state_->buffers.find(*index_buffer_, state_->device_id);
    if (indices == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "draw_indexed: the bound index buffer was destroyed");
    }
    const std::uint64_t indices_in_buffer = indices->desc.size_bytes / index_stride_bytes(index_format_);
    if (static_cast<std::uint64_t>(first_index) + index_count > indices_in_buffer) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed: the index range is outside the bound index buffer");
    }
    if (!vertex_buffers_[0]) {
        return fail(RhiErrorCode::InvalidState, "draw_indexed: vertex buffer slot 0 must be bound");
    }
    if (const RhiStatus checked = check_graphics_state("draw_indexed", PipelineKind::Graphics); !checked.ok()) {
        return checked;
    }
    if (const RhiStatus checked = check_vertex_and_index_state("draw_indexed"); !checked.ok()) {
        return checked;
    }
    return core::success();
}

// Checks the bound vertex and index buffers, the indirect argument buffer (if any), and that the record fits in it.
RhiStatus ValidationDevice::Recorder::check_vertex_and_index_state(std::string_view op) {
    const std::string name(op);
    if (pipeline_) {
        const PipelineRecord* pipeline = state_->pipelines.find(*pipeline_, state_->device_id);
        if (pipeline != nullptr && pipeline->per_instance && !vertex_buffers_[1]) {
            return fail(RhiErrorCode::InvalidState, name + ": the pipeline reads per-instance input, so vertex buffer slot 1 must be bound");
        }
    }
    for (const auto& slot : vertex_buffers_) {
        if (slot) {
            const BufferRecord* record = state_->buffers.find(*slot, state_->device_id);
            if (record == nullptr || record->state != ResourceState::VertexBuffer) {
                return fail(RhiErrorCode::StateMismatch, name + ": a bound vertex buffer is not in the VertexBuffer state");
            }
        }
    }
    if (index_buffer_) {
        const BufferRecord* record = state_->buffers.find(*index_buffer_, state_->device_id);
        if (record == nullptr || record->state != ResourceState::IndexBuffer) {
            return fail(RhiErrorCode::StateMismatch, name + ": the bound index buffer is not in the IndexBuffer state");
        }
    }
    return core::success();
}

RhiStatus ValidationDevice::Recorder::draw_indexed_indirect(BufferHandle args, std::uint64_t args_offset, std::uint32_t draw_count) {
    if (const RhiStatus open = require_recording("draw_indexed_indirect"); !open.ok()) {
        return open;
    }
    if (draw_count == 0) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed_indirect: draw count must be greater than zero");
    }
    const BufferRecord* record = state_->buffers.find(args, state_->device_id);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "draw_indexed_indirect: argument buffer handle is invalid or destroyed");
    }
    if (!has_flags(record->desc.usage, BufferUsage::IndirectArgument) || record->state != ResourceState::IndirectArgument) {
        return fail(RhiErrorCode::StateMismatch, "draw_indexed_indirect: argument buffer needs IndirectArgument usage and state");
    }
    if (args_offset % 4 != 0 || args_offset > record->desc.size_bytes ||
        static_cast<std::uint64_t>(draw_count) * kDrawIndexedIndirectStride > record->desc.size_bytes - args_offset) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed_indirect: arguments do not fit in the buffer");
    }
    if (const RhiStatus checked = check_graphics_state("draw_indexed_indirect", PipelineKind::Graphics); !checked.ok()) {
        return checked;
    }
    if (const RhiStatus checked = check_vertex_and_index_state("draw_indexed_indirect"); !checked.ok()) {
        return checked;
    }
    use(key_of(ResourceKind::Buffer, args));
    return core::success();
}

RhiStatus ValidationDevice::Recorder::draw_indexed_indirect_count(BufferHandle args, std::uint64_t args_offset, BufferHandle count,
                                                                  std::uint64_t count_offset, std::uint32_t max_draw_count) {
    if (const RhiStatus open = require_recording("draw_indexed_indirect_count"); !open.ok()) {
        return open;
    }
    if (!state_->report.enabled(Feature::IndirectCount)) {
        return fail(RhiErrorCode::FeatureDisabled, "draw_indexed_indirect_count: the IndirectCount feature is not enabled");
    }
    if (max_draw_count == 0) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed_indirect_count: max draw count must be greater than zero");
    }
    const BufferRecord* args_record = state_->buffers.find(args, state_->device_id);
    const BufferRecord* count_record = state_->buffers.find(count, state_->device_id);
    if (args_record == nullptr || count_record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "draw_indexed_indirect_count: argument or count buffer handle is invalid");
    }
    for (const BufferRecord* record : {args_record, count_record}) {
        if (!has_flags(record->desc.usage, BufferUsage::IndirectArgument) || record->state != ResourceState::IndirectArgument) {
            return fail(RhiErrorCode::StateMismatch, "draw_indexed_indirect_count: buffers need IndirectArgument usage and state");
        }
    }
    if (args_offset % 4 != 0 || args_offset > args_record->desc.size_bytes ||
        static_cast<std::uint64_t>(max_draw_count) * kDrawIndexedIndirectStride > args_record->desc.size_bytes - args_offset) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed_indirect_count: arguments do not fit in the buffer");
    }
    if (count_offset % 4 != 0 || count_offset > count_record->desc.size_bytes ||
        kIndirectCountSize > count_record->desc.size_bytes - count_offset) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed_indirect_count: count does not fit in its buffer");
    }
    if (const RhiStatus checked = check_graphics_state("draw_indexed_indirect_count", PipelineKind::Graphics); !checked.ok()) {
        return checked;
    }
    if (const RhiStatus checked = check_vertex_and_index_state("draw_indexed_indirect_count"); !checked.ok()) {
        return checked;
    }
    use(key_of(ResourceKind::Buffer, args));
    use(key_of(ResourceKind::Buffer, count));
    return core::success();
}

RhiStatus ValidationDevice::Recorder::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (const RhiStatus open = require_recording("dispatch"); !open.ok()) {
        return open;
    }
    if (x == 0 || y == 0 || z == 0) {
        return fail(RhiErrorCode::OutOfRange, "dispatch: group counts must be greater than zero");
    }
    return check_compute_state("dispatch");
}

RhiStatus ValidationDevice::Recorder::build_bottom_level(AccelerationStructureHandle, BuildMode) {
    return core::failure(ray_tracing_error(state_->report, "build_bottom_level"));
}

RhiStatus ValidationDevice::Recorder::build_top_level(AccelerationStructureHandle, BuildMode) {
    return core::failure(ray_tracing_error(state_->report, "build_top_level"));
}

RhiStatus ValidationDevice::Recorder::set_ray_tracing_pipeline(RayTracingPipelineHandle) {
    return core::failure(ray_tracing_error(state_->report, "set_ray_tracing_pipeline"));
}

RhiStatus ValidationDevice::Recorder::dispatch_rays(std::uint32_t, std::uint32_t, std::uint32_t) {
    return core::failure(ray_tracing_error(state_->report, "dispatch_rays"));
}

RhiStatus ValidationDevice::Recorder::upscale(Upscaler&, const UpscaleDispatch&) {
    if (const RhiStatus open = require_recording("upscale"); !open.ok()) {
        return open;
    }
    return fail(RhiErrorCode::Unsupported, "upscale: the reference device runs no FSR 2 upscale");
}

RhiStatus ValidationDevice::Recorder::dispatch_mesh(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (const RhiStatus open = require_recording("dispatch_mesh"); !open.ok()) {
        return open;
    }
    if (!state_->report.enabled(Feature::MeshShaders)) {
        return fail(RhiErrorCode::FeatureDisabled, "dispatch_mesh: the MeshShaders feature is not enabled");
    }
    if (x == 0 || y == 0 || z == 0) {
        return fail(RhiErrorCode::OutOfRange, "dispatch_mesh: group counts must be greater than zero");
    }
    return check_graphics_state("dispatch_mesh", PipelineKind::Mesh);
}

RhiStatus ValidationDevice::Recorder::copy_buffer(BufferHandle source, std::uint64_t source_offset, BufferHandle destination,
                                                  std::uint64_t destination_offset, std::uint64_t size) {
    if (const RhiStatus open = require_recording("copy_buffer"); !open.ok()) {
        return open;
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "copy_buffer: copies cannot run inside a render pass");
    }
    if (size == 0) {
        return fail(RhiErrorCode::OutOfRange, "copy_buffer: size must be greater than zero");
    }
    if (source == destination) {
        return fail(RhiErrorCode::InvalidDescriptor, "copy_buffer: source and destination must be different buffers");
    }
    const BufferRecord* src = state_->buffers.find(source, state_->device_id);
    const BufferRecord* dst = state_->buffers.find(destination, state_->device_id);
    if (src == nullptr || dst == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "copy_buffer: buffer handle is invalid or destroyed");
    }
    if (!has_flags(src->desc.usage, BufferUsage::CopySource) || src->state != ResourceState::CopySource) {
        return fail(RhiErrorCode::StateMismatch, "copy_buffer: source needs CopySource usage and state");
    }
    if (!has_flags(dst->desc.usage, BufferUsage::CopyDestination) || dst->state != ResourceState::CopyDestination) {
        return fail(RhiErrorCode::StateMismatch, "copy_buffer: destination needs CopyDestination usage and state");
    }
    if (source_offset > src->desc.size_bytes || size > src->desc.size_bytes - source_offset ||
        destination_offset > dst->desc.size_bytes || size > dst->desc.size_bytes - destination_offset) {
        return fail(RhiErrorCode::OutOfRange, "copy_buffer: range is outside a buffer");
    }
    use(key_of(ResourceKind::Buffer, source));
    use(key_of(ResourceKind::Buffer, destination));
    return core::success();
}

RhiStatus ValidationDevice::Recorder::copy_texture_to_buffer(TextureHandle source, BufferHandle destination,
                                                             std::uint64_t destination_offset) {
    if (const RhiStatus open = require_recording("copy_texture_to_buffer"); !open.ok()) {
        return open;
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "copy_texture_to_buffer: copies cannot run inside a render pass");
    }
    const TextureRecord* src = state_->textures.find(source, state_->device_id);
    const BufferRecord* dst = state_->buffers.find(destination, state_->device_id);
    if (src == nullptr || dst == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "copy_texture_to_buffer: handle is invalid or destroyed");
    }
    if (bytes_per_pixel(src->desc.format) == 0) {
        return fail(RhiErrorCode::InvalidDescriptor, "copy_texture_to_buffer: the texture format has no copyable color texels");
    }
    if (!has_flags(src->desc.usage, TextureUsage::CopySource) || src->state != ResourceState::CopySource) {
        return fail(RhiErrorCode::StateMismatch, "copy_texture_to_buffer: source needs CopySource usage and state");
    }
    if (!has_flags(dst->desc.usage, BufferUsage::CopyDestination) || dst->state != ResourceState::CopyDestination) {
        return fail(RhiErrorCode::StateMismatch, "copy_texture_to_buffer: destination needs CopyDestination usage and state");
    }
    if (destination_offset % kTexturePlacementAlignment != 0) {
        return fail(RhiErrorCode::InvalidDescriptor, "copy_texture_to_buffer: destination offset must be a multiple of 512 bytes");
    }
    // The footprint is every row at the aligned pitch, with the last row only as long as its texels.
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(src->desc.width) * bytes_per_pixel(src->desc.format);
    const std::uint64_t footprint = copy_row_pitch(src->desc.width, src->desc.format) * (src->desc.height - 1) + row_bytes;
    if (destination_offset > dst->desc.size_bytes || footprint > dst->desc.size_bytes - destination_offset) {
        return fail(RhiErrorCode::OutOfRange, "copy_texture_to_buffer: the footprint is outside the destination buffer");
    }
    use(key_of(ResourceKind::Texture, source));
    use(key_of(ResourceKind::Buffer, destination));
    return core::success();
}

RhiStatus ValidationDevice::Recorder::copy_buffer_to_texture(BufferHandle source, std::uint64_t source_offset, TextureHandle destination) {
    if (const RhiStatus open = require_recording("copy_buffer_to_texture"); !open.ok()) {
        return open;
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "copy_buffer_to_texture: copies cannot run inside a render pass");
    }
    const BufferRecord* src = state_->buffers.find(source, state_->device_id);
    const TextureRecord* dst = state_->textures.find(destination, state_->device_id);
    if (src == nullptr || dst == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "copy_buffer_to_texture: handle is invalid or destroyed");
    }
    if (bytes_per_pixel(dst->desc.format) == 0) {
        return fail(RhiErrorCode::InvalidDescriptor, "copy_buffer_to_texture: the texture format has no copyable color texels");
    }
    if (!has_flags(src->desc.usage, BufferUsage::CopySource) || src->state != ResourceState::CopySource) {
        return fail(RhiErrorCode::StateMismatch, "copy_buffer_to_texture: source needs CopySource usage and state");
    }
    if (!has_flags(dst->desc.usage, TextureUsage::CopyDestination) || dst->state != ResourceState::CopyDestination) {
        return fail(RhiErrorCode::StateMismatch, "copy_buffer_to_texture: destination needs CopyDestination usage and state");
    }
    if (source_offset % kTexturePlacementAlignment != 0) {
        return fail(RhiErrorCode::InvalidDescriptor, "copy_buffer_to_texture: source offset must be a multiple of 512 bytes");
    }
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(dst->desc.width) * bytes_per_pixel(dst->desc.format);
    const std::uint64_t footprint = copy_row_pitch(dst->desc.width, dst->desc.format) * (dst->desc.height - 1) + row_bytes;
    if (source_offset > src->desc.size_bytes || footprint > src->desc.size_bytes - source_offset) {
        return fail(RhiErrorCode::OutOfRange, "copy_buffer_to_texture: the footprint is outside the source buffer");
    }
    use(key_of(ResourceKind::Buffer, source));
    use(key_of(ResourceKind::Texture, destination));
    return core::success();
}

RhiStatus ValidationDevice::Recorder::write_timestamp(QueryPoolHandle pool, std::uint32_t index) {
    if (const RhiStatus open = require_recording("write_timestamp"); !open.ok()) {
        return open;
    }
    if (!state_->report.enabled(Feature::TimestampQueries)) {
        return fail(RhiErrorCode::FeatureDisabled, "write_timestamp: the TimestampQueries feature is not enabled");
    }
    const QueryRecord* query = state_->query_pools.find(pool, state_->device_id);
    if (query == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_timestamp: query pool handle is invalid or destroyed");
    }
    if (index >= query->values.size()) {
        return fail(RhiErrorCode::OutOfRange, "write_timestamp: index is outside the query pool");
    }
    const ResourceKey key = key_of(ResourceKind::QueryPool, pool);
    use(key);
    timestamps_.push_back(TimestampWrite{key, index});
    return core::success();
}

RhiStatus ValidationDevice::Recorder::close() {
    if (const RhiStatus open = require_recording("close"); !open.ok()) {
        return open;
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "close: a render pass is still active");
    }
    list_state_ = ListState::Closed;
    return core::success();
}

}  // namespace engine::rhi
