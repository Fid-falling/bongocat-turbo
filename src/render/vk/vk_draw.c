#include "vk_internal.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>
#include <string.h>

#ifdef BONGO_CAT_HAS_VULKAN

#include "bongo_cat_vk_shaders.h"

/* 顶点布局必须与 src/render/vk/shaders/*.vert 的 location 顺序一致 */
typedef struct OverlayVertex {
    float x, y, u, v;
} OverlayVertex;

typedef struct PointerVertex {
    float x, y, u, v, r, g, b, a;
} PointerVertex;

typedef struct OverlayPush {
    float reference[2];
    float image_size[2];
    int mirror;
    int erase_left;
    int erase_right;
} OverlayPush;

typedef struct PointerPush {
    int textured;
} PointerPush;

#define BONGO_CAT_VK_ARENA_BYTES (256u * 1024u)

/* OpenGL 后端 overlay.c 里那个四边形: NDC 位置 + 写死的 UV, 三角形带 */
static const OverlayVertex k_overlay_quad[4] = {
    {-1.0f, -1.0f, 0.0f, 1.0f},
    { 1.0f, -1.0f, 1.0f, 1.0f},
    {-1.0f,  1.0f, 0.0f, 0.0f},
    { 1.0f,  1.0f, 1.0f, 0.0f}
};

static void destroy_pipelines(BongoCatVk *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return;
    if (vk->overlay_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, vk->overlay_pipeline, NULL);
        vk->overlay_pipeline = VK_NULL_HANDLE;
    }
    if (vk->pointer_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, vk->pointer_pipeline, NULL);
        vk->pointer_pipeline = VK_NULL_HANDLE;
    }
    if (vk->overlay_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, vk->overlay_layout, NULL);
        vk->overlay_layout = VK_NULL_HANDLE;
    }
    if (vk->pointer_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, vk->pointer_layout, NULL);
        vk->pointer_layout = VK_NULL_HANDLE;
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

static VkPipelineLayout create_layout(BongoCatVk *vk, uint32_t push_size) {
    VkPushConstantRange range = {0};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = push_size;
    VkPipelineLayoutCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    create.setLayoutCount = 1;
    create.pSetLayouts = &vk->texture_layout;
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
    VkPrimitiveTopology topology) {
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
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = attribute_count;
        input.pVertexAttributeDescriptions = attributes;
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
        /* 预乘 alpha: 与 OpenGL 路径的 GL_ONE / GL_ONE_MINUS_SRC_ALPHA 一致 */
        VkPipelineColorBlendAttachmentState blend = {0};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
    destroy_pipelines(vk);

    VkVertexInputBindingDescription overlay_binding = {0};
    overlay_binding.binding = 0;
    overlay_binding.stride = sizeof(OverlayVertex);
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
    pointer_binding.stride = sizeof(PointerVertex);
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

    vk->overlay_layout = create_layout(vk, (uint32_t)sizeof(OverlayPush));
    vk->pointer_layout = create_layout(vk, (uint32_t)sizeof(PointerPush));
    if (vk->overlay_layout == VK_NULL_HANDLE || vk->pointer_layout == VK_NULL_HANDLE) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "[vulkan] 管线布局创建失败");
        destroy_pipelines(vk);
        return;
    }
    vk->overlay_pipeline = build_pipeline(vk, vk->overlay_layout,
        bongo_cat_spv_overlay_vert, sizeof(bongo_cat_spv_overlay_vert),
        bongo_cat_spv_overlay_frag, sizeof(bongo_cat_spv_overlay_frag),
        overlay_binding, 2, overlay_attributes, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    vk->pointer_pipeline = build_pipeline(vk, vk->pointer_layout,
        bongo_cat_spv_pointer_vert, sizeof(bongo_cat_spv_pointer_vert),
        bongo_cat_spv_pointer_frag, sizeof(bongo_cat_spv_pointer_frag),
        pointer_binding, 3, pointer_attributes, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    if (vk->overlay_pipeline == VK_NULL_HANDLE || vk->pointer_pipeline == VK_NULL_HANDLE) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "[vulkan] 图形管线创建失败");
        destroy_pipelines(vk);
    }
}

bool bongo_cat_vk_create_draw_resources(BongoCatVk *vk, BongoCatError *error) {
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        BongoCatVkArena *arena = &vk->arenas[i];
        VkBufferCreateInfo buffer = {0};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = BONGO_CAT_VK_ARENA_BYTES;
        buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(vk->device, &buffer, NULL, &arena->buffer) != VK_SUCCESS) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "Vulkan 顶点缓冲创建失败");
            return false;
        }
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(vk->device, arena->buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties;
        vkGetPhysicalDeviceMemoryProperties(vk->physical, &memory_properties);
        const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = UINT32_MAX;
        for (uint32_t j = 0; j < memory_properties.memoryTypeCount; ++j) {
            if ((requirements.memoryTypeBits & (1u << j)) &&
                (memory_properties.memoryTypes[j].propertyFlags & wanted) == wanted) {
                type = j;
                break;
            }
        }
        if (type == UINT32_MAX) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "没有主机可见的显存类型可用于顶点缓冲");
            return false;
        }
        VkMemoryAllocateInfo allocate = {0};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (vkAllocateMemory(vk->device, &allocate, NULL, &arena->memory) != VK_SUCCESS ||
            vkBindBufferMemory(vk->device, arena->buffer, arena->memory, 0) != VK_SUCCESS ||
            vkMapMemory(vk->device, arena->memory, 0, VK_WHOLE_SIZE, 0,
                (void **)&arena->mapped) != VK_SUCCESS) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "Vulkan 顶点缓冲映射失败");
            return false;
        }
        arena->capacity = BONGO_CAT_VK_ARENA_BYTES;
        arena->offset = 0;
    }
    bongo_cat_vk_rebuild_pipelines(vk);
    if (vk->overlay_pipeline == VK_NULL_HANDLE || vk->pointer_pipeline == VK_NULL_HANDLE) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Vulkan 图形管线不可用");
        return false;
    }
    return true;
}

void bongo_cat_vk_destroy_draw_resources(BongoCatVk *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return;
    destroy_pipelines(vk);
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        BongoCatVkArena *arena = &vk->arenas[i];
        if (arena->mapped) {
            vkUnmapMemory(vk->device, arena->memory);
            arena->mapped = NULL;
        }
        if (arena->buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(vk->device, arena->buffer, NULL);
            arena->buffer = VK_NULL_HANDLE;
        }
        if (arena->memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, arena->memory, NULL);
            arena->memory = VK_NULL_HANDLE;
        }
        arena->capacity = 0;
        arena->offset = 0;
    }
}

static bool arena_push(BongoCatVk *vk, const void *data, size_t size,
    VkDeviceSize *offset) {
    BongoCatVkArena *arena = &vk->arenas[vk->frame_index];
    if (!arena->mapped || arena->offset + size > arena->capacity) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 顶点缓冲不足 (需要 %zu 字节, 剩余 %zu)", size,
            arena->capacity - arena->offset);
        return false;
    }
    *offset = arena->offset;
    memcpy(arena->mapped + arena->offset, data, size);
    arena->offset += size;
    return true;
}

bool bongo_cat_vk_draw_overlay_quad(BongoCatVk *vk, BongoCatVkTexture *texture,
    bool mirror, int reference_width, int reference_height, bool erase_left,
    bool erase_right) {
    if (!vk || !vk->frame_active || !texture) return false;
    if (vk->overlay_pipeline == VK_NULL_HANDLE) return false;
    VkDeviceSize offset = 0;
    if (!arena_push(vk, k_overlay_quad, sizeof(k_overlay_quad), &offset)) return false;
    OverlayPush push = {{0}};
    push.reference[0] = (float)reference_width;
    push.reference[1] = (float)reference_height;
    push.image_size[0] = (float)texture->width;
    push.image_size[1] = (float)texture->height;
    push.mirror = mirror ? 1 : 0;
    push.erase_left = erase_left ? 1 : 0;
    push.erase_right = erase_right ? 1 : 0;
    VkCommandBuffer command = vk->frames[vk->frame_index].command;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->overlay_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        vk->overlay_layout, 0, 1, &texture->descriptor, 0, NULL);
    vkCmdPushConstants(command, vk->overlay_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        (uint32_t)sizeof(push), &push);
    VkDeviceSize vertex_offset = offset;
    vkCmdBindVertexBuffers(command, 0, 1, &vk->arenas[vk->frame_index].buffer,
        &vertex_offset);
    vkCmdDraw(command, 4, 1, 0, 0);
    return true;
}

bool bongo_cat_vk_draw_triangles(BongoCatVk *vk, BongoCatVkTexture *texture,
    const float *vertices, unsigned int vertex_count, bool textured) {
    if (!vk || !vk->frame_active || !vertices || vertex_count < 3) return false;
    if (vk->pointer_pipeline == VK_NULL_HANDLE) return false;
    if (textured && !texture) return false;
    VkDeviceSize offset = 0;
    size_t bytes = (size_t)vertex_count * sizeof(PointerVertex);
    if (!arena_push(vk, vertices, bytes, &offset)) return false;
    PointerPush push = {0};
    push.textured = textured ? 1 : 0;
    VkCommandBuffer command = vk->frames[vk->frame_index].command;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->pointer_pipeline);
    /* 着色器里 texture() 是静态引用, 未贴图时也要绑定一个描述符集 */
    VkDescriptorSet descriptor = textured ? texture->descriptor :
        vk->fallback_texture->descriptor;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        vk->pointer_layout, 0, 1, &descriptor, 0, NULL);
    vkCmdPushConstants(command, vk->pointer_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        (uint32_t)sizeof(push), &push);
    VkDeviceSize vertex_offset = offset;
    vkCmdBindVertexBuffers(command, 0, 1, &vk->arenas[vk->frame_index].buffer,
        &vertex_offset);
    vkCmdDraw(command, vertex_count, 1, 0, 0);
    return true;
}

#endif /* BONGO_CAT_HAS_VULKAN */
