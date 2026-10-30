/* Cubism SDK Vulkan 缓冲分配池的接口。
 *
 * 只有两处使用者:
 *   1. CMake 期被补丁替换的 SDK 源 (CubismClass_Vulkan.cpp) —— CubismBufferVulkan 的三个方法
 *      调到这里;
 *   2. src/live2d/cubism_vulkan_buffer_pool.cpp 的实现。
 * 为什么要单独一个头: 补丁后的源文件要 include 它, 而它不能把 SDK 的头拖进来 (SDK 的
 * CubismBufferVulkan 是私有的类声明)。 */
#ifndef BONGO_CAT_CUBISM_VULKAN_BUFFER_POOL_H
#define BONGO_CAT_CUBISM_VULKAN_BUFFER_POOL_H

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 建缓冲并从池里切一块内存绑上去。失败时 *buffer_out / *memory_out 都是 VK_NULL_HANDLE。
   与 SDK 原实现的行为差异只有一个: 内存不属于这个缓冲, 而是属于池 (释放见下面)。 */
void bongo_cat_vulkan_buffer_create(VkDevice device, VkPhysicalDevice physical,
    VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
    VkBuffer *buffer_out, VkDeviceMemory *memory_out);

/* 缓冲对应的持久映射地址 (宿主可见内存才非空)。池块在进程内一直映射着, 不再逐缓冲映射。 */
void *bongo_cat_vulkan_buffer_mapped(VkBuffer buffer);

/* 只销毁缓冲、把区间还给池; 全部缓冲都释放后池会把整块内存还给驱动。
   幂等: 同一个缓冲重复调用只生效一次。 */
void bongo_cat_vulkan_buffer_release(VkDevice device, VkBuffer buffer);

#ifdef __cplusplus
}
#endif

#endif
