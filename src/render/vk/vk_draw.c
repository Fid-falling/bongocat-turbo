#include "vk_internal.h"
#include <math.h>
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>
#include <string.h>

#ifdef BONGO_CAT_HAS_VULKAN

/* 顶点布局必须与 src/render/vk/shaders/*.vert 的 location 顺序一致 */
typedef struct OverlayVertex {
    float x, y, u, v;
} OverlayVertex;

typedef struct PointerVertex {
    float x, y, u, v, r, g, b, a;
} PointerVertex;

/* 径向菜单的顶点: y 向上为正 (与 OpenGL 约定一致, 着色器末尾取反) */
typedef struct DialVertexIn {
    float x, y, u, v;
    unsigned char r, g, b, a;
} DialVertexIn;

#define BONGO_CAT_VK_ARENA_BYTES (256u * 1024u)
#define BONGO_CAT_VK_INDEX_ARENA_BYTES (128u * 1024u)
/*
 * 径向菜单的顶点缓冲: 一帧实测约 6 万个顶点, 展开成指针格式 (32 字节) 接近
 * 2 MB, 通用缓冲的 256 KB 远远不够 —— 之前菜单在 Vulkan 下整片空白就是这里
 * 静默失败。4 MB 留一倍余量。
 */
#define BONGO_CAT_VK_DIAL_ARENA_BYTES (4u * 1024u * 1024u)
/* Nuklear 的 UIVertex: pos(2f) + uv(2f) + color(RGBA8) */
#define BONGO_CAT_VK_UI_VERTEX_BYTES 20u

/* OpenGL 后端 overlay.c 里那个四边形: NDC 位置 + 写死的 UV, 三角形带 */
static const OverlayVertex k_overlay_quad[4] = {
    {-1.0f, -1.0f, 0.0f, 1.0f},
    { 1.0f, -1.0f, 1.0f, 1.0f},
    {-1.0f,  1.0f, 0.0f, 0.0f},
    { 1.0f,  1.0f, 1.0f, 0.0f}
};

static bool create_arena(BongoCatVk *vk, BongoCatVkArena *arena, size_t bytes,
    VkBufferUsageFlags usage, BongoCatError *error, const char *what);
static void destroy_arena(BongoCatVk *vk, BongoCatVkArena *arena);

bool bongo_cat_vk_create_draw_resources(BongoCatVk *vk, BongoCatError *error) {
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        if (!create_arena(vk, &vk->arenas[i], BONGO_CAT_VK_ARENA_BYTES,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, error, "顶点缓冲") ||
            !create_arena(vk, &vk->index_arenas[i], BONGO_CAT_VK_INDEX_ARENA_BYTES,
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT, error, "索引缓冲") ||
            !create_arena(vk, &vk->dial_arenas[i], BONGO_CAT_VK_DIAL_ARENA_BYTES,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, error, "菜单顶点缓冲"))
            return false;
    }
    /* 回读缓冲 (命中小块 + 整帧) 见 vk_readback.c */
    if (!bongo_cat_vk_create_readbacks(vk, error)) return false;
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
    /* 管线与布置在 vk_pipeline.c, 这里只释放缓冲 */
    bongo_cat_vk_destroy_pipelines(vk);
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        destroy_arena(vk, &vk->arenas[i]);
        destroy_arena(vk, &vk->index_arenas[i]);
        destroy_arena(vk, &vk->dial_arenas[i]);
    }
    bongo_cat_vk_destroy_readbacks(vk);
}

static bool arena_push_into(BongoCatVkArena *arena, const void *data, size_t size,
    VkDeviceSize *offset) {
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

static bool arena_push(BongoCatVk *vk, const void *data, size_t size,
    VkDeviceSize *offset) {
    return arena_push_into(&vk->arenas[vk->frame_index], data, size, offset);
}

static bool create_arena(BongoCatVk *vk, BongoCatVkArena *arena, size_t bytes,
    VkBufferUsageFlags usage, BongoCatError *error, const char *what) {
    VkBufferCreateInfo buffer = {0};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = bytes;
    buffer.usage = usage;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk->device, &buffer, NULL, &arena->buffer) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "%s创建失败", what);
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
            "没有主机可见的显存类型可用于%s", what);
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
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "%s映射失败", what);
        return false;
    }
    arena->capacity = bytes;
    arena->offset = 0;
    return true;
}

static void destroy_arena(BongoCatVk *vk, BongoCatVkArena *arena) {
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

bool bongo_cat_vk_draw_overlay_quad(BongoCatVk *vk, BongoCatVkTexture *texture,
    bool mirror, int reference_width, int reference_height, bool erase_left,
    bool erase_right, bool premultiply, bool keycap_only) {
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
    push.premultiply = premultiply ? 1 : 0;
    push.keycap_only = keycap_only ? 1 : 0;
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

bool bongo_cat_vk_draw_dial(BongoCatVk *vk, BongoCatVkTexture *texture,
    const void *vertices, unsigned int vertex_count, int width, int height) {
    if (!vk || !vk->frame_active || !vertices || vertex_count < 3) return false;
    if (vk->dial_pipeline == VK_NULL_HANDLE || !texture) return false;
    if (width <= 0 || height <= 0) return false;
    /* 菜单顶点就是管线输入格式 (pos 2f + uv 2f + RGBA8), 坐标是窗口逻辑像素,
       整批推进专用缓冲后用 scale/offset 映射到裁剪空间 (与 ui.vert 一致) */
    BongoCatVkArena *arena = &vk->dial_arenas[vk->frame_index];
    size_t bytes = (size_t)vertex_count * sizeof(DialVertexIn);
    VkDeviceSize offset = 0;
    if (!arena_push_into(arena, vertices, bytes, &offset)) return false;
    UiPush push = {{0}};
    push.scale[0] = 2.0f / (float)width;
    push.scale[1] = 2.0f / (float)height;
    push.offset[0] = -1.0f;
    push.offset[1] = -1.0f;
    /* 菜单里既有 RGBA 图标也有单通道字形图集, 见 dial.frag */
    push.color_texture = texture->alpha_mask ? 0 : 1;
    VkCommandBuffer command = vk->frames[vk->frame_index].command;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->dial_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        vk->ui_layout, 0, 1, &texture->descriptor, 0, NULL);
    vkCmdPushConstants(command, vk->ui_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        (uint32_t)sizeof(push), &push);
    VkDeviceSize vertex_offset = offset;
    vkCmdBindVertexBuffers(command, 0, 1, &arena->buffer, &vertex_offset);
    vkCmdDraw(command, vertex_count, 1, 0, 0);
    return true;
}

bool bongo_cat_vk_mask_corners(BongoCatVk *vk, int width, int height,
    float radius_percent) {
    if (!vk || !vk->frame_active || width <= 0 || height <= 0) return false;
    if (vk->corner_pipeline == VK_NULL_HANDLE) return false;
    CornerPush push = {0};
    push.width = width;
    push.height = height;
    push.radius_milli = (int)((width < height ? width : height) *
        radius_percent * 10.0f + 0.5f);
    VkCommandBuffer command = vk->frames[vk->frame_index].command;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->corner_pipeline);
    vkCmdPushConstants(command, vk->corner_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        (uint32_t)sizeof(push), &push);
    vkCmdDraw(command, 3, 1, 0, 0);
    return true;
}

/*
 * 渲染精度把这一帧的目标放大到 N 倍: 视口/裁剪的坐标与尺寸都是"窗口像素", 必须按
 * render_extent / extent 等比放大, 否则内容会按 1x 画在 N 倍目标的左上角 (实测画面
 * 就是"只有左下角一块, 其余空白")。
 */
static void scale_extent_factors(const BongoCatVk *vk, float *sx, float *sy) {
    VkExtent2D target = vk->render_extent.width && vk->render_extent.height ?
        vk->render_extent : vk->extent;
    *sx = vk->extent.width ? (float)target.width / (float)vk->extent.width : 1.0f;
    *sy = vk->extent.height ? (float)target.height / (float)vk->extent.height : 1.0f;
}

void bongo_cat_vk_set_viewport(BongoCatVk *vk, int x, int y, int width,
    int height) {
    if (!vk || !vk->frame_active) return;
    VkExtent2D target = vk->render_extent.width && vk->render_extent.height ?
        vk->render_extent : vk->extent;
    float sx = 1.0f, sy = 1.0f;
    scale_extent_factors(vk, &sx, &sy);
    VkViewport viewport = {0.0f, 0.0f, (float)target.width,
        (float)target.height, 0.0f, 1.0f};
    /*
     * 内容视口: 与 OpenGL 的 glViewport 一致地裁剪到交换链范围内。
     * Vulkan 的视口 y 从上方开始, 调用方传的就是左上角坐标。
     */
    if (width > 0 && height > 0 && x < (int)vk->extent.width &&
        y < (int)vk->extent.height) {
        if (x < 0) { width += x; x = 0; }
        if (y < 0) { height += y; y = 0; }
        if (x + width > (int)vk->extent.width) width = (int)vk->extent.width - x;
        if (y + height > (int)vk->extent.height) height = (int)vk->extent.height - y;
        if (width > 0 && height > 0) {
            viewport.x = (float)x * sx;
            viewport.y = (float)y * sy;
            viewport.width = (float)width * sx;
            viewport.height = (float)height * sy;
        }
    }
    vkCmdSetViewport(vk->frames[vk->frame_index].command, 0, 1, &viewport);
}

void bongo_cat_vk_set_scissor(BongoCatVk *vk, int x, int y, int width, int height) {
    if (!vk || !vk->frame_active) return;
    float sx = 1.0f, sy = 1.0f;
    scale_extent_factors(vk, &sx, &sy);
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > (int)vk->extent.width) width = (int)vk->extent.width - x;
    if (y + height > (int)vk->extent.height) height = (int)vk->extent.height - y;
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    VkRect2D scissor = {{(int32_t)lroundf((float)x * sx),
        (int32_t)lroundf((float)y * sy)},
        {(uint32_t)lroundf((float)width * sx), (uint32_t)lroundf((float)height * sy)}};
    vkCmdSetScissor(vk->frames[vk->frame_index].command, 0, 1, &scissor);
}

bool bongo_cat_vk_ui_upload_vertices(BongoCatVk *vk, const void *vertices,
    unsigned int vertex_count) {
    vk->ui_vertices_ready = false;
    if (!vk || !vk->frame_active || !vertices || !vertex_count) return false;
    if (vk->ui_pipeline == VK_NULL_HANDLE) return false;
    VkDeviceSize offset = 0;
    if (!arena_push_into(&vk->arenas[vk->frame_index], vertices,
            (size_t)vertex_count * BONGO_CAT_VK_UI_VERTEX_BYTES, &offset))
        return false;
    vk->ui_vertex_offset = offset;
    vk->ui_vertices_ready = true;
    return true;
}

bool bongo_cat_vk_ui_draw(BongoCatVk *vk, BongoCatVkTexture *texture,
    const void *indices, unsigned int index_count, float scale_x, float scale_y) {
    if (!vk || !vk->frame_active || !texture || !indices || !index_count) return false;
    if (vk->ui_pipeline == VK_NULL_HANDLE || !vk->ui_vertices_ready) return false;
    VkDeviceSize index_offset = 0;
    if (!arena_push_into(&vk->index_arenas[vk->frame_index], indices,
            (size_t)index_count * sizeof(uint16_t), &index_offset))
        return false;
    UiPush push = {{0}};
    push.scale[0] = scale_x;
    push.scale[1] = scale_y;
    push.offset[0] = -1.0f;
    push.offset[1] = -1.0f;
    push.color_texture = texture->alpha_mask ? 0 : 1; /* 见 ui.frag: 遮罩取 R, RGBA 整体采样 */
    VkCommandBuffer command = vk->frames[vk->frame_index].command;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->ui_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->ui_layout,
        0, 1, &texture->descriptor, 0, NULL);
    vkCmdPushConstants(command, vk->ui_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        (uint32_t)sizeof(push), &push);
    VkDeviceSize vertex_offset = vk->ui_vertex_offset;
    vkCmdBindVertexBuffers(command, 0, 1, &vk->arenas[vk->frame_index].buffer,
        &vertex_offset);
    vkCmdBindIndexBuffer(command, vk->index_arenas[vk->frame_index].buffer,
        index_offset, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(command, index_count, 1, 0, 0, 0);
    return true;
}

int bongo_cat_vk_max_texture_dimension(const BongoCatVk *vk) {
    if (!vk || vk->physical == VK_NULL_HANDLE) return 0;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(vk->physical, &properties);
    return (int)properties.limits.maxImageDimension2D;
}

bool bongo_cat_vk_native_info(const BongoCatVk *vk, BongoCatVkNativeInfo *info) {
    if (!vk || !info || vk->device == VK_NULL_HANDLE) return false;
    memset(info, 0, sizeof(*info));
    info->instance = (void *)vk->instance;
    info->device = (void *)vk->device;
    info->physical_device = (void *)vk->physical;
    info->command_pool = (void *)vk->command_pool;
    info->queue = (void *)vk->queue;
    info->queue_family = vk->queue_family;
    /* 这个后端不做深度缓冲, 于是深度格式填 VK_FORMAT_UNDEFINED(0): 组件按它建管线时
       会走"无深度附件"的路径, 而不是拿一个假格式去建。 */
    info->depth_format = 0;
    /* 动态入口: 组件不链接 volk, 需要新函数时从这两个 loader 入口取。
       vkGetInstanceProcAddr 对 instance 级与 device 级函数都有效, device 版本更省一层。 */
    info->get_instance_proc = (void *(*)(const char *))
        vkGetInstanceProcAddr(vk->instance, "vkGetInstanceProcAddr");
    info->get_device_proc = (void *(*)(const char *))
        vkGetDeviceProcAddr(vk->device, "vkGetDeviceProcAddr");
    info->width = (int)vk->extent.width;
    info->height = (int)vk->extent.height;
    info->image_format = (int)vk->format;
    info->image_count = (int)vk->image_count;
    /* SDK 的 SetConstantSettings 需要一个默认目标视图 (交换链第 0 张) */
    if (vk->image_count > 0) info->image_view = (void *)vk->views[0];
    return true;
}

#endif /* BONGO_CAT_HAS_VULKAN */
