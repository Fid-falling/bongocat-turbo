#include "vk_internal.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>

#ifdef BONGO_CAT_HAS_VULKAN

#include "bongo_cat_vk_shaders.h"

/*
 * 图形管线。GLSL 在配置阶段由 glslc 编成 SPIR-V 并嵌成 C 数组
 * (见 cmake/Vulkan.cmake), 这里只负责把它们组装成管线。
 *
 * 两条混合模式:
 *  - 预乘 alpha (叠加层 / 指针): ONE, ONE_MINUS_SRC_ALPHA
 *  - 覆盖率相乘 (圆角遮罩):     ZERO, SRC_ALPHA
 */

void bongo_cat_vk_destroy_pipelines(BongoCatVk *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return;
    if (vk->overlay_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, vk->overlay_pipeline, NULL);
        vk->overlay_pipeline = VK_NULL_HANDLE;
    }
    if (vk->pointer_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, vk->pointer_pipeline, NULL);
        vk->pointer_pipeline = VK_NULL_HANDLE;
    }
    if (vk->corner_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, vk->corner_pipeline, NULL);
        vk->corner_pipeline = VK_NULL_HANDLE;
    }
    if (vk->ui_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, vk->ui_pipeline, NULL);
        vk->ui_pipeline = VK_NULL_HANDLE;
    }
    if (vk->dial_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, vk->dial_pipeline, NULL);
        vk->dial_pipeline = VK_NULL_HANDLE;
    }
    if (vk->overlay_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, vk->overlay_layout, NULL);
        vk->overlay_layout = VK_NULL_HANDLE;
    }
    if (vk->pointer_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, vk->pointer_layout, NULL);
        vk->pointer_layout = VK_NULL_HANDLE;
    }
    if (vk->corner_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, vk->corner_layout, NULL);
        vk->corner_layout = VK_NULL_HANDLE;
    }
    if (vk->ui_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, vk->ui_layout, NULL);
        vk->ui_layout = VK_NULL_HANDLE;
    }
}

static VkShaderModule create_module(BongoCatVk *vk, const uint32_t *code,
    size_t bytes) {
    VkShaderModuleCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create.codeSize = bytes;
    create.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(vk->device, &create, NULL, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

static VkPipelineLayout create_layout(BongoCatVk *vk, uint32_t push_size,
    bool with_texture) {
    VkPushConstantRange range = {0};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = push_size;
    VkPipelineLayoutCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (with_texture) {
        create.setLayoutCount = 1;
        create.pSetLayouts = &vk->texture_layout;
    }
    create.pushConstantRangeCount = 1;
    create.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(vk->device, &create, NULL, &layout) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return layout;
}

static VkPipeline build_pipeline(BongoCatVk *vk, VkPipelineLayout layout,
    const uint32_t *vertex_code, size_t vertex_bytes, const uint32_t *fragment_code,
    size_t fragment_bytes, VkVertexInputBindingDescription binding,
    uint32_t attribute_count, const VkVertexInputAttributeDescription *attributes,
    VkPrimitiveTopology topology, BongoCatVkBlend blend_mode) {
    VkShaderModule vertex = create_module(vk, vertex_code, vertex_bytes);
    VkShaderModule fragment = create_module(vk, fragment_code, fragment_bytes);
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vertex != VK_NULL_HANDLE && fragment != VK_NULL_HANDLE) {
        VkPipelineShaderStageCreateInfo stages[2] = {{0}};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo input = {0};
        input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        input.vertexBindingDescriptionCount = attribute_count ? 1 : 0;
        input.pVertexBindingDescriptions = attribute_count ? &binding : NULL;
        input.vertexAttributeDescriptionCount = attribute_count;
        input.pVertexAttributeDescriptions = attribute_count ? attributes : NULL;
        VkPipelineInputAssemblyStateCreateInfo assembly = {0};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = topology;
        VkPipelineViewportStateCreateInfo viewport = {0};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster = {0};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample = {0};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend = {0};
        blend.blendEnable = VK_TRUE;
        switch (blend_mode) {
        case BONGO_CAT_VK_BLEND_MULTIPLY:
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            break;
        case BONGO_CAT_VK_BLEND_ALPHA:
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BONGO_CAT_VK_BLEND_PREMULTIPLIED:
        default:
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        }
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo color = {0};
        color.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color.attachmentCount = 1;
        color.pAttachments = &blend;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic = {0};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = states;
        VkGraphicsPipelineCreateInfo create = {0};
        create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        create.stageCount = 2;
        create.pStages = stages;
        create.pVertexInputState = &input;
        create.pInputAssemblyState = &assembly;
        create.pViewportState = &viewport;
        create.pRasterizationState = &raster;
        create.pMultisampleState = &multisample;
        create.pColorBlendState = &color;
        create.pDynamicState = &dynamic;
        create.layout = layout;
        create.renderPass = vk->render_pass;
        create.subpass = 0;
        if (vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &create, NULL,
                &pipeline) != VK_SUCCESS)
            pipeline = VK_NULL_HANDLE;
    }
    if (vertex != VK_NULL_HANDLE) vkDestroyShaderModule(vk->device, vertex, NULL);
    if (fragment != VK_NULL_HANDLE) vkDestroyShaderModule(vk->device, fragment, NULL);
    return pipeline;
}

void bongo_cat_vk_rebuild_pipelines(BongoCatVk *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return;
    if (vk->arenas[0].buffer == VK_NULL_HANDLE) return;
    if (vk->render_pass == VK_NULL_HANDLE) return;
    bongo_cat_vk_destroy_pipelines(vk);

    VkVertexInputBindingDescription overlay_binding = {0};
    overlay_binding.binding = 0;
    overlay_binding.stride = 16;   /* vec2 位置 + vec2 UV */
    overlay_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription overlay_attributes[2] = {{0}};
    overlay_attributes[0].location = 0;
    overlay_attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    overlay_attributes[0].offset = 0;
    overlay_attributes[1].location = 1;
    overlay_attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    overlay_attributes[1].offset = 8;

    VkVertexInputBindingDescription pointer_binding = {0};
    pointer_binding.binding = 0;
    pointer_binding.stride = 32;   /* 位置 + UV + 顶点色 */
    pointer_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription pointer_attributes[3] = {{0}};
    pointer_attributes[0].location = 0;
    pointer_attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    pointer_attributes[0].offset = 0;
    pointer_attributes[1].location = 1;
    pointer_attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    pointer_attributes[1].offset = 8;
    pointer_attributes[2].location = 2;
    pointer_attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    pointer_attributes[2].offset = 16;

    vk->overlay_layout = create_layout(vk, (uint32_t)sizeof(OverlayPush), true);
    vk->pointer_layout = create_layout(vk, (uint32_t)sizeof(PointerPush), true);
    vk->corner_layout = create_layout(vk, (uint32_t)sizeof(CornerPush), false);
    vk->ui_layout = create_layout(vk, (uint32_t)sizeof(UiPush), true);
    if (vk->overlay_layout == VK_NULL_HANDLE || vk->pointer_layout == VK_NULL_HANDLE ||
        vk->corner_layout == VK_NULL_HANDLE || vk->ui_layout == VK_NULL_HANDLE) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "[vulkan] 管线布局创建失败");
        bongo_cat_vk_destroy_pipelines(vk);
        return;
    }
    vk->overlay_pipeline = build_pipeline(vk, vk->overlay_layout,
        bongo_cat_spv_overlay_vert, sizeof(bongo_cat_spv_overlay_vert),
        bongo_cat_spv_overlay_frag, sizeof(bongo_cat_spv_overlay_frag),
        overlay_binding, 2, overlay_attributes, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        BONGO_CAT_VK_BLEND_PREMULTIPLIED);
    vk->pointer_pipeline = build_pipeline(vk, vk->pointer_layout,
        bongo_cat_spv_pointer_vert, sizeof(bongo_cat_spv_pointer_vert),
        bongo_cat_spv_pointer_frag, sizeof(bongo_cat_spv_pointer_frag),
        pointer_binding, 3, pointer_attributes, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        BONGO_CAT_VK_BLEND_PREMULTIPLIED);
    VkVertexInputBindingDescription no_binding = {0};
    vk->corner_pipeline = build_pipeline(vk, vk->corner_layout,
        bongo_cat_spv_corner_vert, sizeof(bongo_cat_spv_corner_vert),
        bongo_cat_spv_corner_frag, sizeof(bongo_cat_spv_corner_frag),
        no_binding, 0, NULL, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        BONGO_CAT_VK_BLEND_MULTIPLY);

    /* 设置界面: pos(2f) + uv(2f) + color(RGBA8) */
    VkVertexInputBindingDescription ui_binding = {0};
    ui_binding.binding = 0;
    ui_binding.stride = 20;
    ui_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription ui_attributes[3] = {{0}};
    ui_attributes[0].location = 0;
    ui_attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    ui_attributes[0].offset = 0;
    ui_attributes[1].location = 1;
    ui_attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    ui_attributes[1].offset = 8;
    ui_attributes[2].location = 2;
    ui_attributes[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    ui_attributes[2].offset = 16;
    vk->ui_pipeline = build_pipeline(vk, vk->ui_layout,
        bongo_cat_spv_ui_vert, sizeof(bongo_cat_spv_ui_vert),
        bongo_cat_spv_ui_frag, sizeof(bongo_cat_spv_ui_frag),
        ui_binding, 3, ui_attributes, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        BONGO_CAT_VK_BLEND_ALPHA);

    /*
     * 径向菜单: 顶点格式与设置界面相同 (pos 2f + uv 2f + color RGBA8), 但片元
     * 走普通 RGBA 采样, 所以单独一条管线 —— 复用 ui_layout 的 push constant
     * (同样是 scale/offset) 与描述符布局。
     */
    vk->dial_pipeline = build_pipeline(vk, vk->ui_layout,
        bongo_cat_spv_dial_vert, sizeof(bongo_cat_spv_dial_vert),
        bongo_cat_spv_dial_frag, sizeof(bongo_cat_spv_dial_frag),
        ui_binding, 3, ui_attributes, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        BONGO_CAT_VK_BLEND_PREMULTIPLIED);

    if (vk->overlay_pipeline == VK_NULL_HANDLE || vk->pointer_pipeline == VK_NULL_HANDLE ||
        vk->corner_pipeline == VK_NULL_HANDLE || vk->ui_pipeline == VK_NULL_HANDLE ||
        vk->dial_pipeline == VK_NULL_HANDLE) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "[vulkan] 图形管线创建失败");
        bongo_cat_vk_destroy_pipelines(vk);
    }
}

#endif /* BONGO_CAT_HAS_VULKAN */
