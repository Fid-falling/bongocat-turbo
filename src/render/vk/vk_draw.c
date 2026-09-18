#include "vk_internal.h"
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

#define BONGO_CAT_VK_ARENA_BYTES (256u * 1024u)

/* OpenGL 后端 overlay.c 里那个四边形: NDC 位置 + 写死的 UV, 三角形带 */
static const OverlayVertex k_overlay_quad[4] = {
    {-1.0f, -1.0f, 0.0f, 1.0f},
    { 1.0f, -1.0f, 1.0f, 1.0f},
    {-1.0f,  1.0f, 0.0f, 0.0f},
    { 1.0f,  1.0f, 1.0f, 0.0f}
};

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
    /* 命中测试回读缓冲: 指针附近的一小块 RGBA8 */
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        BongoCatVkReadback *readback = &vk->readback[i];
        const VkDeviceSize bytes = BONGO_CAT_VK_HIT_TILE * BONGO_CAT_VK_HIT_TILE * 4u;
        VkBufferCreateInfo buffer = {0};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = bytes;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(vk->device, &buffer, NULL, &readback->buffer) != VK_SUCCESS) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "Vulkan 回读缓冲创建失败");
            return false;
        }
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(vk->device, readback->buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties;
        vkGetPhysicalDeviceMemoryProperties(vk->physical, &memory_properties);
        uint32_t type = UINT32_MAX;
        for (uint32_t j = 0; j < memory_properties.memoryTypeCount; ++j) {
            if ((requirements.memoryTypeBits & (1u << j)) &&
                (memory_properties.memoryTypes[j].propertyFlags &
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                type = j;
                break;
            }
        }
        VkMemoryAllocateInfo allocate = {0};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (type == UINT32_MAX ||
            vkAllocateMemory(vk->device, &allocate, NULL, &readback->memory) !=
                VK_SUCCESS ||
            vkBindBufferMemory(vk->device, readback->buffer, readback->memory, 0) !=
                VK_SUCCESS ||
            vkMapMemory(vk->device, readback->memory, 0, VK_WHOLE_SIZE, 0,
                (void **)&readback->mapped) != VK_SUCCESS) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "Vulkan 回读缓冲映射失败");
            return false;
        }
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
    /* 管线与布置在 vk_pipeline.c, 这里只释放缓冲 */
    bongo_cat_vk_destroy_pipelines(vk);
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
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        BongoCatVkReadback *readback = &vk->readback[i];
        if (readback->mapped) {
            vkUnmapMemory(vk->device, readback->memory);
            readback->mapped = NULL;
        }
        if (readback->buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(vk->device, readback->buffer, NULL);
            readback->buffer = VK_NULL_HANDLE;
        }
        if (readback->memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, readback->memory, NULL);
            readback->memory = VK_NULL_HANDLE;
        }
        readback->requested = readback->pending = false;
    }
    vk->hit_valid = false;
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

#endif /* BONGO_CAT_HAS_VULKAN */
