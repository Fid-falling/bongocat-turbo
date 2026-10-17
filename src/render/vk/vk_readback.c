/*
 * Vulkan 帧回读: 把 GPU 上画好的像素取回主机内存。
 *
 * 两种用途共用"每个帧槽一份"的缓冲, 同一帧里整帧优先:
 *   - 命中测试: 鼠标附近一小块的 alpha, 决定点击该不该穿透宠物 (逐帧都可能用到);
 *   - 整帧回读: 模型封面捕获与帧诊断 (冒烟), 一次几 MB。
 *
 * 语义是异步的, 别当成同步读:
 *   queue_* 只在命令缓冲里排队, 结果要等下一帧 collect 之后才 ready, take 才拿得到
 *   —— 也就是"取到的是上一帧的画面"。take 会清掉 frame_ready, 同一份数据只能取一次。
 *
 * 返回的指针是借来的: 内存清理 (bongo_cat_vk_release_cached_frames) 会释放整帧缓冲,
 * 下次排队时重新分配。要长期用就自己拷一份 (封面路径就是这么做的)。
 */

#include "vk_internal.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>

#ifdef BONGO_CAT_HAS_VULKAN

/*
 * 交换链图像的回读。
 *
 * 两种用途共用一个帧槽缓冲:
 *  - 命中测试: 指针附近 BONGO_CAT_VK_HIT_TILE 见方的一小块, 只取 alpha;
 *  - 整帧回读: 模型封面 / 冒烟诊断 / 审计要完整 RGBA (顶部为第 0 行)。
 * 一帧槽一次只能录一个区域, 所以两者互斥: 整帧请求优先, 命中回读下一帧照常。
 * 缓冲按需扩容 (命中块很小, 整帧要 w*h*4), 数据在"下一次复用该帧槽"时才可读,
 * 也就是对外是一帧延迟的语义。
 */

static void destroy_readback_buffer(BongoCatVk *vk, BongoCatVkReadback *readback) {
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
    readback->whole_frame = false;
    readback->capacity = 0;
}

static bool ensure_readback_buffer(BongoCatVk *vk, BongoCatVkReadback *readback,
    VkDeviceSize bytes) {
    if (!vk || !readback || bytes == 0) return false;
    if (readback->buffer != VK_NULL_HANDLE && readback->capacity >= bytes) return true;
    destroy_readback_buffer(vk, readback);
    VkBufferCreateInfo buffer = {0};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = bytes;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk->device, &buffer, NULL, &readback->buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk->device, readback->buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(vk->physical, &memory_properties);
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
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
        destroy_readback_buffer(vk, readback);
        return false;
    }
    readback->capacity = bytes;
    return true;
}

bool bongo_cat_vk_create_readbacks(BongoCatVk *vk, BongoCatError *error) {
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        const VkDeviceSize bytes =
            (VkDeviceSize)BONGO_CAT_VK_HIT_TILE * BONGO_CAT_VK_HIT_TILE * 4u;
        if (!ensure_readback_buffer(vk, &vk->readback[i], bytes)) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "Vulkan 回读缓冲创建失败");
            return false;
        }
    }
    return true;
}

void bongo_cat_vk_destroy_readbacks(BongoCatVk *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return;
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i)
        destroy_readback_buffer(vk, &vk->readback[i]);
    SDL_free(vk->frame_pixels);
    vk->frame_pixels = NULL;
    vk->frame_width = vk->frame_height = 0;
    vk->frame_ready = false;
    vk->hit_valid = false;
}

void bongo_cat_vk_collect_readback(BongoCatVk *vk) {
    if (!vk) return;
    BongoCatVkReadback *readback = &vk->readback[vk->frame_index];
    if (!readback->pending || !readback->mapped) return;
    readback->pending = false;
    if (readback->width <= 0 || readback->height <= 0) return;
    if (readback->whole_frame) {
        const size_t bytes = (size_t)readback->width * (size_t)readback->height * 4u;
        unsigned char *pixels = SDL_realloc(vk->frame_pixels, bytes);
        if (!pixels) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "[vulkan] 整帧回读缓冲分配失败 (%dx%d)", readback->width,
                readback->height);
            return;
        }
        SDL_memcpy(pixels, readback->mapped, bytes);
        /*
         * 交换链优先用 B8G8R8A8 (见 vk_swapchain.c 的优先格式表), 所以回读到的
         * 字节序是 BGRA; 而消费端 (模型封面写 PNG、帧诊断写 BMP 与分量统计) 一律
         * 按 RGBA 解释。在这里统一换一次 R/B, 让"回读结果 = RGBA"成为本模块的
         * 对外契约 —— 否则封面与诊断预览会红蓝互换 (蓝色渲出来变黄白), 且 GL 路径
         * 是对的 (glReadPixels 直接要 GL_RGBA), 两个后端就对不上了。
         *
         * 踩过的坑: 当初验收只比了"不透明像素占比 / 有色像素占比"这类统计量, 而
         * 互换通道不改变这些比例, 于是漏掉了; 比较颜色请比对分量均值, 别只比占比。
         */
        if (vk->format == VK_FORMAT_B8G8R8A8_UNORM ||
            vk->format == VK_FORMAT_B8G8R8A8_SRGB) {
            const size_t count =
                (size_t)readback->width * (size_t)readback->height;
            for (size_t i = 0; i < count; ++i) {
                unsigned char *pixel = pixels + i * 4u;
                const unsigned char swap = pixel[0];
                pixel[0] = pixel[2];
                pixel[2] = swap;
            }
        }
        vk->frame_pixels = pixels;
        vk->frame_width = readback->width;
        vk->frame_height = readback->height;
        vk->frame_ready = true;
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[vulkan] 整帧回读完成: %dx%d 原点=(%d,%d)", readback->width,
            readback->height, readback->origin_x, readback->origin_y);
        return;
    }
    for (int y = 0; y < readback->height; ++y) {
        for (int x = 0; x < readback->width; ++x) {
            vk->hit_alpha[y * readback->width + x] =
                readback->mapped[((size_t)y * (size_t)readback->width +
                    (size_t)x) * 4 + 3];
        }
    }
    vk->hit_x = readback->origin_x;
    vk->hit_y = readback->origin_y;
    vk->hit_width = readback->width;
    vk->hit_height = readback->height;
    if (!vk->hit_valid)
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[vulkan] 首次命中测试回读成功: 区域=%dx%d 原点=(%d,%d)",
            readback->width, readback->height, readback->origin_x,
            readback->origin_y);
    vk->hit_valid = true;
}

/*
 * 释放"下次还能再拿回来"的整帧回读缓冲。内存清理 (memory_policy) 时调用:
 * 封面/诊断用完的几 MB 没必要一直常驻, 下一次整帧回读会按需重新分配。
 * 命中测试的固定小块不动 (它是每帧都可能用到的热路径)。
 */
void bongo_cat_vk_release_cached_frames(BongoCatVk *vk) {
    if (!vk) return;
    if (!vk->frame_pixels) return;
    SDL_free(vk->frame_pixels);
    vk->frame_pixels = NULL;
    vk->frame_width = vk->frame_height = 0;
    vk->frame_ready = false;
}

/*
 * 模型切换完成的瞬间调用: 作废整帧回读的已缓存结果与所有待采集 (pending/requested)
 * 的整帧样本。
 *
 * 回读是异步的 (queue->下一帧 collect->take), 而提交切换那一帧的主循环 capture 会
 * 立即渲染并 take —— 若此刻还留着一份切换前模型的旧回读, take 就会把旧模型的画面写进
 * 新模型的封面。这里在每次切换时把"上一模型"可能残留的整帧像素全部扔掉, 让切换后的
 * take 只能等到新模型渲染的那一帧, 也就是"切换完成之后再捕获"。
 */
void bongo_cat_vk_discard_pending_frame_sample(BongoCatVk *vk) {
    if (!vk) return;
    vk->frame_ready = false;
    vk->frame_width = vk->frame_height = 0;
    for (uint32_t i = 0; i < BONGO_CAT_VK_FRAMES_IN_FLIGHT; ++i) {
        if (vk->readback[i].whole_frame) {
            vk->readback[i].requested = false;
            vk->readback[i].pending = false;
        }
    }
}

bool bongo_cat_vk_queue_hit_sample(BongoCatVk *vk, int x, int y) {
    if (!vk || !vk->frame_active || !vk->transfer_source) return false;
    const int tile = BONGO_CAT_VK_HIT_TILE;
    if ((int)vk->extent.width < tile || (int)vk->extent.height < tile) return false;
    if (x < 0 || y < 0 || x >= (int)vk->extent.width || y >= (int)vk->extent.height)
        return false;
    BongoCatVkReadback *readback = &vk->readback[vk->frame_index];
    /* 整帧请求优先 (封面/诊断), 命中回读下一帧照常更新 */
    if (readback->requested && readback->whole_frame) return false;
    int origin_x = x - tile / 2;
    int origin_y = y - tile / 2;
    if (origin_x < 0) origin_x = 0;
    if (origin_y < 0) origin_y = 0;
    if (origin_x + tile > (int)vk->extent.width)
        origin_x = (int)vk->extent.width - tile;
    if (origin_y + tile > (int)vk->extent.height)
        origin_y = (int)vk->extent.height - tile;
    readback->requested = true;
    readback->whole_frame = false;
    readback->origin_x = origin_x;
    readback->origin_y = origin_y;
    readback->width = tile;
    readback->height = tile;
    return true;
}

bool bongo_cat_vk_hit_alpha(const BongoCatVk *vk, int x, int y,
    unsigned char *alpha) {
    if (!vk || !vk->hit_valid || !alpha) return false;
    if (x < vk->hit_x || y < vk->hit_y ||
        x >= vk->hit_x + vk->hit_width || y >= vk->hit_y + vk->hit_height)
        return false;
    *alpha = vk->hit_alpha[(size_t)(y - vk->hit_y) * (size_t)vk->hit_width +
        (size_t)(x - vk->hit_x)];
    return true;
}

bool bongo_cat_vk_queue_frame_sample(BongoCatVk *vk, int x, int y, int w, int h) {
    if (!vk || !vk->frame_active || !vk->transfer_source) return false;
    if (w <= 0 || h <= 0) return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if ((uint64_t)x + (uint64_t)w > vk->extent.width)
        w = (int)vk->extent.width - x;
    if ((uint64_t)y + (uint64_t)h > vk->extent.height)
        h = (int)vk->extent.height - y;
    if (w <= 0 || h <= 0) return false;
    BongoCatVkReadback *readback = &vk->readback[vk->frame_index];
    const VkDeviceSize bytes = (VkDeviceSize)w * (VkDeviceSize)h * 4u;
    if (!ensure_readback_buffer(vk, readback, bytes)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 整帧回读缓冲扩容失败 (%dx%d)", w, h);
        return false;
    }
    readback->requested = true;
    readback->whole_frame = true;
    readback->origin_x = x;
    readback->origin_y = y;
    readback->width = w;
    readback->height = h;
    return true;
}

bool bongo_cat_vk_take_frame_sample(BongoCatVk *vk, int *w, int *h,
    const unsigned char **pixels) {
    if (!vk || !vk->frame_ready || !vk->frame_pixels) return false;
    if (w) *w = vk->frame_width;
    if (h) *h = vk->frame_height;
    if (pixels) *pixels = vk->frame_pixels;
    vk->frame_ready = false;   /* 一次性消费, 下一次回读会覆盖 */
    return true;
}

#endif /* BONGO_CAT_HAS_VULKAN */
