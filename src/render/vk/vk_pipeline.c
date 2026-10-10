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
 *
 * 三条必须同时成立的耦合, 改任何一处都要对着另两处看:
 *  1. 顶点输入布局 (binding 步长 + attribute 的 location/format/offset) 必须与 SPIR-V
 *     里 layout(location=...) 的声明一致 —— 不一致不会报错, 只会画出位置/颜色全错的
 *     几何, 是最难查的一类问题;
 *  2. 推送常量的字节大小必须等于 vk_internal.h 里 OverlayPush/PointerPush/CornerPush/
 *     UiPush 的 sizeof, 也必须等于着色器里 std430 块的大小 (少一个字段就是静默错值);
 *  3. 管线与 vk->render_pass 强绑定, 所以交换链一重建 (格式/渲染通道换了) 就必须整批
 *     重建, 见 bongo_cat_vk_rebuild_pipelines。
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

/*
 * 布局里的推送常量同时给顶点与片元两个阶段 (两边都可能读), 这是刻意的: 拆成两个
 * range 会让每个着色器各占一段偏移, 而我们的 push 结构体本来就是给两边共用的。
 * with_texture=false 的布局 (圆角遮罩) 没有任何描述符集 —— 绘制时也不要给它绑,
 * 绑了属于校验层报错的未定义用法。
 */
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

/*
 * 组装一条图形管线。几个刻意的取值:
 *  - cullMode = NONE: 2D 图元的绕序不统一 (三角形扇/带展开后方向可能翻转), 开剔除会
 *    让部分图元凭空消失;
 *  - 混合恒开, 连圆角遮罩也不例外 (它靠 ZERO/SRC_ALPHA 把 RGB 与 alpha 一起乘覆盖率,
 *    与 OpenGL 路径的 blend_func_separate 逐项对应);
 *  - 动态状态只有 viewport/scissor, 管线里的 viewportCount/scissorCount 只是占位,
 *    真实矩形每次绘制由 vkCmdSetViewport/vkCmdSetScissor 给 (见 vk_draw.c);
 *  - 顶点输入用 attribute_count 一个值同时决定 binding 与 attributes 是否存在, 所以
 *    无顶点输入的管线 (圆角遮罩) 传 0/NULL 即可, 不要只关掉其中一个;
 *  - 两个 shader module 在返回前统一销毁, 失败路径也走到 (模块只在建管线时需要)。
 */
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

/*
 * 整批重建。两个早退守卫不是冗余:
 *  - arenas[0].buffer 为空说明绘制资源还没建 —— 启动顺序里交换链先于 draw_resources
 *    创建, 交换链那次"重建"回调必须在这里安静地退出, 真正建管线的是 create_draw_resources;
 *  - render_pass 为空时建管线必然失败 (管线与渲染通道强绑定)。
 *
 * 失败处理刻意只打日志 + 拆干净, 不向上返回错误: 调用方是交换链重建路径, 那里没有
 * 可用的错误通道。代价是失败后所有 vk->*_pipeline 都是 VK_NULL_HANDLE, 因此绘制路径
 * 必须容忍"管线为空"(跳过这一层) 而不是直接绑定。
 */
void bongo_cat_vk_rebuild_pipelines(BongoCatVk *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return;
    if (vk->arenas[0].buffer == VK_NULL_HANDLE) return;
    if (vk->render_pass == VK_NULL_HANDLE) return;
    bongo_cat_vk_destroy_pipelines(vk);

    VkVertexInputBindingDescription overlay_binding = {0};
    overlay_binding.binding = 0;
    /* 步长必须等于叠加层顶点结构的大小 (vec2 位置 + vec2 UV = 16 字节), 由 vk_draw.c
       那边的顶点写入决定; 改顶点结构就要同步改这里 */
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
    /* 32 字节 = mver 指针层的 PointerVertex (位置 2f + UV 2f + 顶点色 4f, 8 个 float);
       这个布局在 mver_pointer_overlay_draw.c 里按 8 个 float 平铺着写 */
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
