#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/vulkan/vk_device.h"

#include <cstdint>

namespace de::rhi {

namespace {

VkShaderStageFlagBits to_vk(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:
        return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStage::Fragment:
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStage::Compute:
        return VK_SHADER_STAGE_COMPUTE_BIT;
    case ShaderStage::Count:
        break;
    }
    return VK_SHADER_STAGE_ALL;
}

VkPrimitiveTopology to_vk(Topology topology) {
    switch (topology) {
    case Topology::TriangleList:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case Topology::TriangleStrip:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case Topology::LineList:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case Topology::PointList:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkCompareOp to_vk(CompareOp op) {
    switch (op) {
    case CompareOp::Never:
        return VK_COMPARE_OP_NEVER;
    case CompareOp::Less:
        return VK_COMPARE_OP_LESS;
    case CompareOp::Equal:
        return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessEqual:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater:
        return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual:
        return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterEqual:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always:
        return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkPipelineColorBlendAttachmentState to_vk(BlendMode mode) {
    VkPipelineColorBlendAttachmentState state{
        .blendEnable = mode == BlendMode::Opaque ? VK_FALSE : VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    if (mode == BlendMode::Premultiplied) {
        state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    } else if (mode == BlendMode::Additive) {
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    }
    return state;
}

// A shader module for the length of pipeline creation; the pipeline keeps what it needs.
class ScopedShaderModule {
public:
    ScopedShaderModule(VkDevice device, const ShaderCode& code) : device_(device) {
        if (code.code.empty() || code.code.size() % 4 != 0 ||
            reinterpret_cast<uintptr_t>(code.code.data()) % alignof(uint32_t) != 0) {
            return; // module_ stays null: the caller reports it
        }
        const VkShaderModuleCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = code.code.size(),
            .pCode = reinterpret_cast<const uint32_t*>(code.code.data()),
        };
        if (vkCreateShaderModule(device_, &info, nullptr, &module_) != VK_SUCCESS) {
            module_ = VK_NULL_HANDLE;
        }
    }
    ~ScopedShaderModule() {
        if (module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, module_, nullptr);
        }
    }
    ScopedShaderModule(const ScopedShaderModule&) = delete;
    ScopedShaderModule& operator=(const ScopedShaderModule&) = delete;
    ScopedShaderModule(ScopedShaderModule&&) = delete;
    ScopedShaderModule& operator=(ScopedShaderModule&&) = delete;

    VkShaderModule get() const { return module_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkShaderModule module_ = VK_NULL_HANDLE;
};

VkPipelineShaderStageCreateInfo stage_info(ShaderStage stage, VkShaderModule module) {
    return {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = to_vk(stage),
        .module = module,
        .pName = "main",
    };
}

const char* name_or_empty(const char* name) {
    return name != nullptr ? name : "";
}

} // namespace

core::Result<PipelineHandle> Device::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    DE_PROFILE_ZONE("rhi create graphics pipeline");
    DeviceImpl& impl = *impl_;
    if (desc.vertex.stage != ShaderStage::Vertex || desc.fragment.stage != ShaderStage::Fragment) {
        return core::fail(core::ErrorCode::InvalidArgument, "a graphics pipeline needs a vertex and a fragment shader");
    }
    if (desc.color_count > kMaxColorAttachments || (desc.color_count == 0 && desc.depth_format == Format::Unknown)) {
        return core::fail(core::ErrorCode::InvalidArgument,
                          "a graphics pipeline needs 1 to 8 color formats or a depth format");
    }
    if (desc.depth_format != Format::Unknown && !format_info(desc.depth_format).depth) {
        return core::fail(core::ErrorCode::InvalidArgument, "the depth format isn't a depth format");
    }

    const ScopedShaderModule vertex(impl.device, desc.vertex);
    const ScopedShaderModule fragment(impl.device, desc.fragment);
    if (vertex.get() == VK_NULL_HANDLE || fragment.get() == VK_NULL_HANDLE) {
        DE_LOG_ERROR("render", "pipeline '%s': the driver rejected the shader code", name_or_empty(desc.debug_name));
        return core::fail(core::ErrorCode::InvalidArgument, "the shader code is not valid for this backend");
    }
    const VkPipelineShaderStageCreateInfo stages[] = {stage_info(ShaderStage::Vertex, vertex.get()),
                                                      stage_info(ShaderStage::Fragment, fragment.get())};

    VkFormat color_formats[kMaxColorAttachments] = {};
    VkPipelineColorBlendAttachmentState blends[kMaxColorAttachments] = {};
    for (uint32_t i = 0; i < desc.color_count; ++i) {
        color_formats[i] = to_vk_format(desc.color_formats[i]);
        blends[i] = to_vk(desc.blend[i]);
    }
    const bool has_stencil = desc.depth_format != Format::Unknown && format_info(desc.depth_format).stencil;
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = desc.color_count,
        .pColorAttachmentFormats = color_formats,
        .depthAttachmentFormat = to_vk_format(desc.depth_format),
        .stencilAttachmentFormat = has_stencil ? to_vk_format(desc.depth_format) : VK_FORMAT_UNDEFINED,
    };

    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo input_assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = to_vk(desc.topology),
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    // Vulkan judges winding as it appears on screen, and the flipped viewport (negative height)
    // shows +Y-up clip space upright, so counter-clockwise in clip space stays counter-clockwise.
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = to_vk(desc.cull),
        .frontFace = desc.front_counter_clockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = to_vk(desc.depth_compare),
    };
    const VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = desc.color_count,
        .pAttachments = blends,
    };
    // Cull mode is dynamic (core in Vulkan 1.3): bind_pipeline sets the pipeline's own, and
    // set_cull_mode overrides it, so one shader serves culled and double-sided draws.
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_CULL_MODE};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3,
        .pDynamicStates = dynamic_states,
    };
    const VkGraphicsPipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = impl.pipeline_layout,
    };
    Pipeline pipeline{.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS, .cull = to_vk(desc.cull)};
    const VkResult result =
        vkCreateGraphicsPipelines(impl.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline.pipeline);
    if (result != VK_SUCCESS) {
        DE_LOG_ERROR("render", "pipeline '%s': %s", name_or_empty(desc.debug_name), vk_result_name(result));
        return core::fail(core::ErrorCode::InvalidArgument, "the graphics pipeline could not be created");
    }
    impl.set_debug_name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeline.pipeline), desc.debug_name);
    return impl.pipelines.insert(pipeline);
}

core::Result<PipelineHandle> Device::create_compute_pipeline(const ComputePipelineDesc& desc) {
    DE_PROFILE_ZONE("rhi create compute pipeline");
    DeviceImpl& impl = *impl_;
    if (desc.shader.stage != ShaderStage::Compute) {
        return core::fail(core::ErrorCode::InvalidArgument, "a compute pipeline needs a compute shader");
    }
    const ScopedShaderModule shader(impl.device, desc.shader);
    if (shader.get() == VK_NULL_HANDLE) {
        DE_LOG_ERROR("render", "pipeline '%s': the driver rejected the shader code", name_or_empty(desc.debug_name));
        return core::fail(core::ErrorCode::InvalidArgument, "the shader code is not valid for this backend");
    }
    const VkComputePipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stage_info(ShaderStage::Compute, shader.get()),
        .layout = impl.pipeline_layout,
    };
    Pipeline pipeline{.bind_point = VK_PIPELINE_BIND_POINT_COMPUTE};
    const VkResult result =
        vkCreateComputePipelines(impl.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline.pipeline);
    if (result != VK_SUCCESS) {
        DE_LOG_ERROR("render", "pipeline '%s': %s", name_or_empty(desc.debug_name), vk_result_name(result));
        return core::fail(core::ErrorCode::InvalidArgument, "the compute pipeline could not be created");
    }
    impl.set_debug_name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeline.pipeline), desc.debug_name);
    return impl.pipelines.insert(pipeline);
}

} // namespace de::rhi
