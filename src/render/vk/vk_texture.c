#include "vk_internal.h"
#include "bongo_cat/image.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BONGO_CAT_HAS_VULKAN

bool bongo_cat_vk_create_texture_resources(BongoCatVk *vk, BongoCatError *error) {
    VkDescriptorSetLayoutBinding binding = {0};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout = {0};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 1;
    layout.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(vk->device, &layout, NULL,
            &vk->texture_layout) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Vulkan 描述符布局创建失败");
        return false;
    }
    VkDescriptorPoolSize size = {0};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 64;
    VkDescriptorPoolCreateInfo pool = {0};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = 64;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (vkCreateDescriptorPool(vk->device, &pool, NULL,
            &vk->descriptor_pool) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Vulkan 描述符池创建失败");
        return false;
    }
    VkSamplerCreateInfo sampler = {0};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    /* 与 src/media/image.c 里普通图片纹理的 GL_LINEAR + CLAMP_TO_EDGE 对齐 */
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0.0f;
    if (vkCreateSampler(vk->device, &sampler, NULL, &vk->sampler) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 采样器创建失败");
        return false;
    }
    const unsigned char white[4] = {255, 255, 255, 255};
    vk->fallback_texture = bongo_cat_vk_texture_from_pixels(vk, white, 1, 1,
        "fallback", error);
    return vk->fallback_texture != NULL;
}

void bongo_cat_vk_destroy_texture_resources(BongoCatVk *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return;
    if (vk->fallback_texture) {
        bongo_cat_vk_texture_destroy(vk, vk->fallback_texture);
        vk->fallback_texture = NULL;
    }
    if (vk->sampler != VK_NULL_HANDLE) {
        vkDestroySampler(vk->device, vk->sampler, NULL);
        vk->sampler = VK_NULL_HANDLE;
    }
    if (vk->descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(vk->device, vk->descriptor_pool, NULL);
        vk->descriptor_pool = VK_NULL_HANDLE;
    }
    if (vk->texture_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vk->device, vk->texture_layout, NULL);
        vk->texture_layout = VK_NULL_HANDLE;
    }
}

static bool create_buffer(BongoCatVk *vk, VkDeviceSize size,
    VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer *buffer,
    VkDeviceMemory *memory) {
    VkBufferCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create.size = size;
    create.usage = usage;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk->device, &create, NULL, buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk->device, *buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(vk->physical, &memory_properties);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(vk->device, &allocate, NULL, memory) != VK_SUCCESS) return false;
    return vkBindBufferMemory(vk->device, *buffer, *memory, 0) == VK_SUCCESS;
}

/* 一次性命令缓冲: 纹理上传 (暂存缓冲 -> 图像) 和布局转换都走这里 */
static VkCommandBuffer begin_upload(BongoCatVk *vk) {
    VkCommandBufferAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = vk->command_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vk->device, &allocate, &command) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return VK_NULL_HANDLE;
    return command;
}

static void end_upload(BongoCatVk *vk, VkCommandBuffer command) {
    if (command == VK_NULL_HANDLE) return;
    if (vkEndCommandBuffer(command) == VK_SUCCESS) {
        VkSubmitInfo submit = {0};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        if (vkQueueSubmit(vk->queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS)
            vkQueueWaitIdle(vk->queue);
    }
    vkFreeCommandBuffers(vk->device, vk->command_pool, 1, &command);
}

static void transition_image(VkCommandBuffer command, VkImage image,
    VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    VkPipelineStageFlags source_stage, destination_stage;
    if (from == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier.srcAccessMask = 0;
        source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    vkCmdPipelineBarrier(command, source_stage, destination_stage, 0, 0, NULL,
        0, NULL, 1, &barrier);
}

BongoCatVkTexture *bongo_cat_vk_texture_from_pixels(BongoCatVk *vk,
    const unsigned char *rgba, int width, int height, const char *label,
    BongoCatError *error) {
    if (!vk || !rgba || width <= 0 || height <= 0) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT, "Vulkan 纹理参数无效");
        return NULL;
    }
    BongoCatVkTexture *texture = calloc(1, sizeof(*texture));
    if (!texture) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY, "Vulkan 纹理状态分配失败");
        return NULL;
    }
    texture->width = width;
    texture->height = height;
    snprintf(texture->label, sizeof(texture->label), "%s", label ? label : "texture");
    VkDeviceSize bytes = (VkDeviceSize)width * (VkDeviceSize)height * 4u;

    VkImageCreateInfo image = {0};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent.width = (uint32_t)width;
    image.extent.height = (uint32_t)height;
    image.extent.depth = 1;
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk->device, &image, NULL, &texture->image) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 纹理图像创建失败");
        bongo_cat_vk_texture_destroy(vk, texture);
        return NULL;
    }
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk->device, texture->image, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(vk->physical, &memory_properties);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags &
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    }
    VkMemoryAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (type == UINT32_MAX ||
        vkAllocateMemory(vk->device, &allocate, NULL, &texture->memory) != VK_SUCCESS ||
        vkBindImageMemory(vk->device, texture->image, texture->memory, 0) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 纹理显存分配失败");
        bongo_cat_vk_texture_destroy(vk, texture);
        return NULL;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    if (!create_buffer(vk, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &staging, &staging_memory)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 暂存缓冲分配失败");
        bongo_cat_vk_texture_destroy(vk, texture);
        return NULL;
    }
    void *mapped = NULL;
    if (vkMapMemory(vk->device, staging_memory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 暂存缓冲映射失败");
        vkDestroyBuffer(vk->device, staging, NULL);
        vkFreeMemory(vk->device, staging_memory, NULL);
        bongo_cat_vk_texture_destroy(vk, texture);
        return NULL;
    }
    memcpy(mapped, rgba, (size_t)bytes);
    vkUnmapMemory(vk->device, staging_memory);

    VkCommandBuffer command = begin_upload(vk);
    if (command == VK_NULL_HANDLE) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 上传命令缓冲失败");
        vkDestroyBuffer(vk->device, staging, NULL);
        vkFreeMemory(vk->device, staging_memory, NULL);
        bongo_cat_vk_texture_destroy(vk, texture);
        return NULL;
    }
    transition_image(command, texture->image, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)width;
    region.imageExtent.height = (uint32_t)height;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(command, staging, texture->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition_image(command, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_upload(vk, command);
    vkDestroyBuffer(vk->device, staging, NULL);
    vkFreeMemory(vk->device, staging_memory, NULL);

    VkImageViewCreateInfo view = {0};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = texture->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vk->device, &view, NULL, &texture->view) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 纹理视图创建失败");
        bongo_cat_vk_texture_destroy(vk, texture);
        return NULL;
    }
    VkDescriptorSetAllocateInfo descriptor = {0};
    descriptor.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor.descriptorPool = vk->descriptor_pool;
    descriptor.descriptorSetCount = 1;
    descriptor.pSetLayouts = &vk->texture_layout;
    if (vkAllocateDescriptorSets(vk->device, &descriptor,
            &texture->descriptor) != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 描述符集分配失败");
        bongo_cat_vk_texture_destroy(vk, texture);
        return NULL;
    }
    VkDescriptorImageInfo info = {0};
    info.sampler = vk->sampler;
    info.imageView = texture->view;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write = {0};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = texture->descriptor;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);
    return texture;
}

BongoCatVkTexture *bongo_cat_vk_texture_load(BongoCatVk *vk, const char *path,
    int *width, int *height, BongoCatError *error) {
    BongoCatImage image = {0};
    if (bongo_cat_image_load(path, &image, error) != BONGO_CAT_OK) return NULL;
    BongoCatVkTexture *texture = bongo_cat_vk_texture_from_pixels(vk,
        image.pixels, image.width, image.height, path, error);
    if (width) *width = image.width;
    if (height) *height = image.height;
    bongo_cat_image_free(&image);
    return texture;
}

void bongo_cat_vk_texture_destroy(BongoCatVk *vk, BongoCatVkTexture *texture) {
    if (!texture) return;
    if (vk && vk->device != VK_NULL_HANDLE) {
        if (texture->descriptor != VK_NULL_HANDLE)
            vkFreeDescriptorSets(vk->device, vk->descriptor_pool, 1,
                &texture->descriptor);
        if (texture->view != VK_NULL_HANDLE)
            vkDestroyImageView(vk->device, texture->view, NULL);
        if (texture->image != VK_NULL_HANDLE)
            vkDestroyImage(vk->device, texture->image, NULL);
        if (texture->memory != VK_NULL_HANDLE)
            vkFreeMemory(vk->device, texture->memory, NULL);
    }
    free(texture);
}

int bongo_cat_vk_texture_width(const BongoCatVkTexture *texture) {
    return texture ? texture->width : 0;
}

int bongo_cat_vk_texture_height(const BongoCatVkTexture *texture) {
    return texture ? texture->height : 0;
}

#endif /* BONGO_CAT_HAS_VULKAN */
