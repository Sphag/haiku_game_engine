// Compute, graphics, and mesh pipelines (US-029, US-033, US-038). A pipeline's root signature has one descriptor table per set, so a bound
// set's heap range is the table. A mesh pipeline is a pipeline state stream (D3D12 has no mesh-specific desc) with an optional amplification
// shader, a mesh shader, and a pixel shader.

#include "dx12/dx12_device.hpp"

#include "engine/core/result.hpp"
#include "engine/rhi/pipeline_rules.hpp"

#include <climits>
#include <vector>

namespace engine::rhi::dx12 {
namespace {

D3D12_DESCRIPTOR_RANGE_TYPE range_type(DescriptorType type) noexcept {
    switch (type) {
        case DescriptorType::ConstantBuffer:
            return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        case DescriptorType::ShaderResource:
            return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        case DescriptorType::UnorderedAccess:
            return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    }
    return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
}

}  // namespace

// Set i is register space i, and a binding's register is its binding index. Ranges are listed in layout order with APPEND, which
// matches the heap order that DescriptorSetObject uses. Every stage may read every table, so visibility is ALL.
RhiResult<ComPtr<ID3D12RootSignature>> build_root_signature(ID3D12Device* device,
                                                           const std::vector<std::vector<DescriptorBindingDesc>>& layouts) {
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> tables(layouts.size());
    std::vector<D3D12_ROOT_PARAMETER> parameters(layouts.size());
    for (std::size_t set = 0; set < layouts.size(); ++set) {
        for (const DescriptorBindingDesc& binding : layouts[set]) {
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = range_type(binding.type);
            range.NumDescriptors = binding.count;
            range.BaseShaderRegister = binding.binding;
            range.RegisterSpace = static_cast<UINT>(set);
            range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            tables[set].push_back(range);
        }
        parameters[set].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[set].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(tables[set].size());
        parameters[set].DescriptorTable.pDescriptorRanges = tables[set].data();
        parameters[set].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    if (const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error); FAILED(hr)) {
        std::string detail = hresult_text(hr);
        if (error) {
            detail += ": ";
            detail.append(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize());
        }
        return fail<ComPtr<ID3D12RootSignature>>(RhiErrorCode::InvalidDescriptor, "create_pipeline: root signature rejected: " + detail);
    }
    ComPtr<ID3D12RootSignature> root;
    if (const HRESULT hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)); FAILED(hr)) {
        return core::failure(hresult_error("create_pipeline: CreateRootSignature", hr));
    }
    return core::success(root);
}

PipelineObject* Dx12Device::find_pipeline(PipelineHandle handle) const {
    return pipelines_.find(handle, device_id_);
}

RhiResult<ID3D12CommandSignature*> Dx12Device::draw_indexed_signature() {
    if (draw_indexed_signature_) {
        return core::success(draw_indexed_signature_.Get());
    }
    // One DrawIndexedInstanced argument, and a record of kDrawIndexedIndirectStride bytes (indexCount, instanceCount, firstIndex,
    // baseVertex, firstInstance). It takes no root arguments, so it needs no root signature.
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = static_cast<UINT>(kDrawIndexedIndirectStride);
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &argument;
    ComPtr<ID3D12CommandSignature> signature;
    if (const HRESULT hr = device_->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)); FAILED(hr)) {
        return core::failure(hresult_error("draw_indexed_indirect: CreateCommandSignature", hr));
    }
    draw_indexed_signature_ = std::move(signature);
    return core::success(draw_indexed_signature_.Get());
}

namespace {

// Graphics state of a pipeline (see PipelineDesc): triangle lists, the cull mode it names, optional alpha blending, and LESS depth tests.
// The fixed state that graphics and mesh pipelines share: blending, culling, front faces, and depth (PipelineDesc).
void fill_fixed_state(const PipelineDesc& desc, D3D12_BLEND_DESC& blend, D3D12_RASTERIZER_DESC& rasterizer, D3D12_DEPTH_STENCIL_DESC& depth) {
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    // Blending is the same for every target. Alpha uses ONE and INV_SRC_ALPHA, so the alpha of a blended pixel is coverage over the
    // destination alpha (PipelineDesc::alpha_blend). Additive adds the source and keeps the destination alpha (additive_blend).
    for (D3D12_RENDER_TARGET_BLEND_DESC& target : blend.RenderTarget) {
        target.BlendEnable = (desc.alpha_blend || desc.additive_blend) ? TRUE : FALSE;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = desc.additive_blend ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA;
        target.DestBlend = desc.additive_blend ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = desc.additive_blend ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE;
        target.DestBlendAlpha = desc.additive_blend ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = desc.cull_mode == CullMode::Back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    // Front faces are counter-clockwise on screen, the glTF convention. Vulkan selects the same faces with VK_FRONT_FACE_CLOCKWISE, because
    // its framebuffer y points down.
    rasterizer.FrontCounterClockwise = TRUE;
    rasterizer.DepthClipEnable = TRUE;
    depth.DepthEnable = desc.depth_format != Format::Unknown ? TRUE : FALSE;
    depth.DepthWriteMask = desc.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depth.StencilEnable = FALSE;
}

// The stream's subobjects are padded to a pointer's alignment, which the D3D12 stream parser requires. MSVC reports that padding as C4324,
// and /WX makes it an error, so the warning is off for these types only (the vendored d3dx12.h's stream types have the same layout).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

// One subobject of a pipeline state stream: its type tag, then its value, aligned to a pointer the way the D3D12 stream parser expects.
template <typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) StreamSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
};

// The mesh pipeline state (US-038). The amplification entry is empty when the pipeline has none.
struct MeshPipelineStream {
    StreamSubobject<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root;
    StreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS> amplification;
    StreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> mesh;
    StreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> pixel;
    StreamSubobject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
    StreamSubobject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer;
    StreamSubobject<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depth;
    StreamSubobject<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT> depth_format;
    StreamSubobject<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> render_targets;
    StreamSubobject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> samples;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

D3D12_SHADER_BYTECODE bytecode_of(const ShaderBinary& shader) noexcept {
    return D3D12_SHADER_BYTECODE{shader.bytecode.data(), shader.bytecode.size()};
}

RhiResult<ComPtr<ID3D12PipelineState>> build_mesh_state(ID3D12Device* device, ID3D12RootSignature* root, const PipelineDesc& desc,
                                                        const ShaderBinary* amplification, const ShaderBinary& mesh, const ShaderBinary& pixel) {
    MeshPipelineStream stream{};
    stream.root.value = root;
    if (amplification != nullptr) {
        stream.amplification.value = bytecode_of(*amplification);
    }
    stream.mesh.value = bytecode_of(mesh);
    stream.pixel.value = bytecode_of(pixel);
    fill_fixed_state(desc, stream.blend.value, stream.rasterizer.value, stream.depth.value);
    stream.depth_format.value = to_dxgi(desc.depth_format);
    stream.render_targets.value.NumRenderTargets = static_cast<UINT>(desc.color_formats.size());
    for (std::size_t i = 0; i < desc.color_formats.size(); ++i) {
        stream.render_targets.value.RTFormats[i] = to_dxgi(desc.color_formats[i]);
    }
    stream.samples.value = DXGI_SAMPLE_DESC{1, 0};

    // The stream is created on ID3D12Device2, which every device with mesh shaders provides.
    ComPtr<ID3D12Device2> device2;
    if (const HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&device2)); FAILED(hr)) {
        return core::failure(hresult_error("create_pipeline: ID3D12Device2", hr));
    }
    const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
    ComPtr<ID3D12PipelineState> pipeline;
    if (const HRESULT hr = device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pipeline)); FAILED(hr)) {
        return core::failure(hresult_error("create_pipeline: CreatePipelineState (mesh)", hr));
    }
    return core::success(pipeline);
}

RhiResult<ComPtr<ID3D12PipelineState>> build_graphics_state(ID3D12Device* device, ID3D12RootSignature* root, const PipelineDesc& desc,
                                                            const ShaderBinary& vertex, const ShaderBinary& pixel) {
    // Attribute semantics are TEXCOORD<location>, so one HLSL source binds on both backends (see VertexAttribute).
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    elements.reserve(desc.vertex_attributes.size());
    for (const VertexAttribute& attribute : desc.vertex_attributes) {
        D3D12_INPUT_ELEMENT_DESC element{};
        element.SemanticName = "TEXCOORD";
        element.SemanticIndex = attribute.location;
        element.Format = to_dxgi(attribute.format);
        element.InputSlot = 0;
        element.AlignedByteOffset = attribute.offset;
        element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        element.InstanceDataStepRate = 0;
        elements.push_back(element);
    }
    // Per-instance input (slot 1). Its element advances once per instance, and DX12 offsets it by the draw's StartInstanceLocation, which is
    // the first_instance an indirect draw sets (SV_InstanceID does not include it).
    for (const VertexAttribute& attribute : desc.instance_attributes) {
        D3D12_INPUT_ELEMENT_DESC element{};
        element.SemanticName = "TEXCOORD";
        element.SemanticIndex = attribute.location;
        element.Format = to_dxgi(attribute.format);
        element.InputSlot = 1;
        element.AlignedByteOffset = attribute.offset;
        element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
        element.InstanceDataStepRate = 1;
        elements.push_back(element);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
    state.pRootSignature = root;
    state.VS.pShaderBytecode = vertex.bytecode.data();
    state.VS.BytecodeLength = vertex.bytecode.size();
    state.PS.pShaderBytecode = pixel.bytecode.data();
    state.PS.BytecodeLength = pixel.bytecode.size();
    fill_fixed_state(desc, state.BlendState, state.RasterizerState, state.DepthStencilState);
    state.SampleMask = UINT_MAX;
    state.InputLayout.pInputElementDescs = elements.empty() ? nullptr : elements.data();
    state.InputLayout.NumElements = static_cast<UINT>(elements.size());
    state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.NumRenderTargets = static_cast<UINT>(desc.color_formats.size());
    for (std::size_t i = 0; i < desc.color_formats.size(); ++i) {
        state.RTVFormats[i] = to_dxgi(desc.color_formats[i]);
    }
    state.DSVFormat = to_dxgi(desc.depth_format);
    state.SampleDesc.Count = 1;
    state.SampleDesc.Quality = 0;

    ComPtr<ID3D12PipelineState> pipeline;
    if (const HRESULT hr = device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&pipeline)); FAILED(hr)) {
        return core::failure(hresult_error("create_pipeline: CreateGraphicsPipelineState", hr));
    }
    return core::success(pipeline);
}

}  // namespace

RhiResult<PipelineHandle> Dx12Device::create_pipeline(const PipelineDesc& desc) {
    if (desc.set_layouts.size() > kMaxDescriptorSetsPerPipeline) {
        return fail<PipelineHandle>(RhiErrorCode::OutOfRange, "create_pipeline: too many descriptor set layouts");
    }
    if (desc.kind == PipelineKind::Compute) {
        if (desc.shaders.size() != 1 || desc.shaders[0].stage != ShaderStage::Compute) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: a compute pipeline needs exactly one compute shader");
        }
        if (!desc.color_formats.empty() || desc.depth_format != Format::Unknown || !desc.vertex_attributes.empty()) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: a compute pipeline has no render targets or vertex input");
        }
    } else if (desc.kind == PipelineKind::Mesh) {
        if (!report_.enabled(Feature::MeshShaders)) {
            return fail<PipelineHandle>(RhiErrorCode::FeatureDisabled, "create_pipeline: mesh pipelines need the MeshShaders feature");
        }
        if (desc.shaders.size() < 2 || desc.shaders.size() > 3) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: a mesh pipeline needs a mesh and a pixel shader, and may add an amplification shader");
        }
        if (!desc.vertex_attributes.empty() || !desc.instance_attributes.empty()) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: a mesh pipeline has no vertex input");
        }
        if (desc.color_formats.size() > kMaxColorTargets) {
            return fail<PipelineHandle>(RhiErrorCode::OutOfRange, "create_pipeline: too many color targets");
        }
        for (const Format format : desc.color_formats) {
            if (format == Format::Unknown || is_depth_format(format)) {
                return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: color targets need a color format");
            }
        }
        if (desc.depth_format != Format::Unknown && !is_depth_format(desc.depth_format)) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: depth target needs a depth format");
        }
        if (const RhiStatus blend = check_blend_state(desc); !blend.ok()) {
            return core::failure(blend.error());
        }
    } else {
        if (desc.shaders.size() != 2) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: a graphics pipeline needs a vertex and a pixel shader");
        }
        if (desc.color_formats.size() > kMaxColorTargets) {
            return fail<PipelineHandle>(RhiErrorCode::OutOfRange, "create_pipeline: too many color targets");
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
    }
    const ShaderBinary* vertex = nullptr;
    const ShaderBinary* pixel = nullptr;
    const ShaderBinary* compute = nullptr;
    const ShaderBinary* mesh = nullptr;
    const ShaderBinary* amplification = nullptr;
    for (const ShaderBinary& shader : desc.shaders) {
        if (shader.entry_point.empty() || shader.bytecode.empty()) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: every shader needs an entry point and bytecode");
        }
        if (shader.stage == ShaderStage::Vertex) {
            vertex = &shader;
        } else if (shader.stage == ShaderStage::Pixel) {
            pixel = &shader;
        } else if (shader.stage == ShaderStage::Compute) {
            compute = &shader;
        } else if (shader.stage == ShaderStage::Mesh) {
            mesh = &shader;
        } else if (shader.stage == ShaderStage::Amplification) {
            amplification = &shader;
        }
    }
    if (desc.kind == PipelineKind::Compute) {
        if (compute == nullptr) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: the shader stages do not match the pipeline kind");
        }
    } else if (desc.kind == PipelineKind::Mesh) {
        if (mesh == nullptr || pixel == nullptr || vertex != nullptr || compute != nullptr) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: the shader stages do not match the pipeline kind");
        }
    } else if (vertex == nullptr || pixel == nullptr || mesh != nullptr || amplification != nullptr) {
        return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: the shader stages do not match the pipeline kind");
    }

    auto object = std::make_shared<PipelineObject>();
    object->kind = desc.kind;
    object->vertex_stride = desc.vertex_stride;
    object->instance_stride = desc.instance_stride;
    for (const DescriptorSetLayoutHandle& layout : desc.set_layouts) {
        const DescriptorSetLayoutObject* source = find_layout(layout);
        if (source == nullptr) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidHandle, "create_pipeline: descriptor set layout handle is invalid or destroyed");
        }
        object->layouts.push_back(source->bindings);
    }

    auto root = build_root_signature(device_.Get(), object->layouts);
    if (!root.ok()) {
        return core::failure(root.error());
    }
    object->root_signature = std::move(root).value();

    if (desc.kind == PipelineKind::Compute) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
        state.pRootSignature = object->root_signature.Get();
        state.CS.pShaderBytecode = compute->bytecode.data();
        state.CS.BytecodeLength = compute->bytecode.size();
        if (const HRESULT hr = device_->CreateComputePipelineState(&state, IID_PPV_ARGS(&object->state)); FAILED(hr)) {
            return core::failure(hresult_error("create_pipeline: CreateComputePipelineState", hr));
        }
    } else if (desc.kind == PipelineKind::Mesh) {
        auto state = build_mesh_state(device_.Get(), object->root_signature.Get(), desc, amplification, *mesh, *pixel);
        if (!state.ok()) {
            return core::failure(state.error());
        }
        object->state = std::move(state).value();
    } else {
        auto state = build_graphics_state(device_.Get(), object->root_signature.Get(), desc, *vertex, *pixel);
        if (!state.ok()) {
            return core::failure(state.error());
        }
        object->state = std::move(state).value();
    }
    set_name(object->root_signature.Get(), desc.debug_name);
    set_name(object->state.Get(), desc.debug_name);
    return core::success(pipelines_.add(std::move(object), device_id_));
}

RhiStatus Dx12Device::destroy(PipelineHandle pipeline) {
    std::shared_ptr<PipelineObject> object = pipelines_.take(pipeline, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: pipeline handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

}  // namespace engine::rhi::dx12
