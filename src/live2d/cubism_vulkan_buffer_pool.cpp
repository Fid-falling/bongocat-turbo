/*
 * Cubism SDK 的 Vulkan 缓冲分配池 (供 CMake 期补丁后的 CubismClass_Vulkan.cpp 调用)。
 *
 * 为什么需要它: SDK 的 CubismBufferVulkan::CreateBuffer 是"一个缓冲一次 vkAllocateMemory"。
 * 一个 313 drawable 的模型会给 staging/vertex/index/uniform 各建一套, 再乘交换链图像数 (3) ——
 * 合计近两千次独立分配。实测 (RTX 4060, 234x234 窗口):
 *   - 21  drawable 的内置模型: 建缓冲 73ms,  提交内存 +87MB
 *   - 313 drawable 的用户模型: 建缓冲 1.1s, 提交内存 +500MB (≈1.6MB/drawable)
 * 差异全在被驱动按块取整的"每次分配"上: 500MB / 1878 次 ≈ 266KB/次, 而实际负载多是几 KB。
 *
 * 做法: 按 (内存类型, 属性) 分组, 每组一次大块分配, 缓冲在其中按对齐子分配; 释放只把区间
 * 还给池子 (带合并), 最后一个缓冲释放完才真的把整块还给驱动。于是:
 *   - 分配次数不变 (vkCreateBuffer 仍然一次一个, 那很便宜), 但 vkAllocateMemory 从近两千次
 *     降到个位数;
 *   - 常驻内存回到"实际负载 + 对齐浪费"的量级;
 *   - 宿主可见内存整块持久映射一次, Map() 只是算出 base+offset (SDK 原来每次 vkMapMemory)。
 *
 * 线程模型: SDK 的 Vulkan 渲染器只在渲染线程上建/销毁缓冲 (本程序的渲染线程只有一个),
 * 这里仍加一把锁 —— 代价相对一次 vkCreateBuffer 可以忽略, 换来的是不必假设调用方线程模型。
 *
 * 复用安全性: 区间只在对应缓冲被 Destroy 之后才回到空闲表。SDK 的退役队列保证旧模型的
 * 缓冲在 3 帧之后才销毁, 所以复用发生在"上一帧的拷贝早已完成"之后。极端情况下若出现一帧
 * 花屏, 先怀疑这里 (可把 reuse 改成"隔一代再复用")。
 */
#include "cubism_vulkan_buffer_pool.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

struct FreeRange {
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

struct Block {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint32_t type_index = 0;
    VkMemoryPropertyFlags properties = 0;
    void *mapped = nullptr;
    /* 已用区间的右端: 新分配优先从这里往后长, 空闲表空了才回卷复用 */
    VkDeviceSize used = 0;
    /* 块里还活着的缓冲数: 归零就能把整块还给驱动 (例如 8K 贴图那块 134MB 的临时 staging) */
    size_t live = 0;
    std::vector<FreeRange> free_ranges;
};

struct Allocation {
    size_t block = 0;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

std::mutex pool_mutex;
std::vector<Block> blocks;
std::unordered_map<VkBuffer, Allocation> allocations;
size_t live_buffers = 0;
/* 单个池块的起点大小: 一个模型的全部小缓冲通常在几十 MB 以内, 8MB 起步够覆盖绝大多数机型 */
constexpr VkDeviceSize kBlockBytes = 8u * 1024u * 1024u;
constexpr VkDeviceSize kGranularity = 64u * 1024u;

VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    if (alignment <= 1) return value;
    return (value + alignment - 1) / alignment * alignment;
}

uint32_t find_memory_type(VkPhysicalDevice physical, uint32_t type_bits,
    VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory = {};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        const bool bit = (type_bits & (1u << index)) != 0;
        const bool match =
            (memory.memoryTypes[index].propertyFlags & properties) == properties;
        if (bit && match) return index;
    }
    return UINT32_MAX;
}

/* 从块里切一块 range 出去: 优先用空闲表 (first fit), 其次从 used 往后长。
   切剩下的头部/尾部只要够一个粒度就重新登记成空闲区间, 小于粒度的碎片直接并进本次分配。 */
bool take_range(Block *block, VkDeviceSize size, VkDeviceSize alignment,
    VkDeviceSize *offset) {
    for (size_t i = 0; i < block->free_ranges.size(); ++i) {
        const FreeRange range = block->free_ranges[i];
        const VkDeviceSize start = align_up(range.offset, alignment);
        if (start + size > range.offset + range.size) continue;
        const VkDeviceSize head = start - range.offset;
        const VkDeviceSize tail = range.offset + range.size - (start + size);
        block->free_ranges.erase(block->free_ranges.begin() + (long)i);
        if (head >= kGranularity) block->free_ranges.push_back({range.offset, head});
        if (tail >= kGranularity)
            block->free_ranges.push_back({start + size, tail});
        *offset = start;
        return true;
    }
    const VkDeviceSize start = align_up(block->used, alignment);
    if (start + size > block->size) return false;
    block->used = start + size;
    *offset = start;
    return true;
}

void return_range(Block *block, VkDeviceSize offset, VkDeviceSize size) {
    if (!size) return;
    block->free_ranges.push_back({offset, size});
    /* 合并相邻区间: 区间数量很少 (≤缓冲种类数), 排序 + 单趟合并足够 */
    std::sort(block->free_ranges.begin(), block->free_ranges.end(),
        [](const FreeRange &a, const FreeRange &b) { return a.offset < b.offset; });
    std::vector<FreeRange> merged;
    for (const FreeRange &range : block->free_ranges) {
        if (!merged.empty() &&
            merged.back().offset + merged.back().size >= range.offset) {
            merged.back().size = std::max(merged.back().size,
                range.offset + range.size - merged.back().offset);
        } else {
            merged.push_back(range);
        }
    }
    block->free_ranges = std::move(merged);
}

/* 全部缓冲都释放了就把整块还给驱动: 换模型时旧模型先销毁, 这里不一定命中 (新模型的缓冲
   已经在了), 但退出/降级路径能收回来, 不会越用越多。 */
void release_blocks_if_idle(VkDevice device) {
    if (live_buffers) return;
    for (Block &block : blocks) {
        if (block.mapped) vkUnmapMemory(device, block.memory);
        vkFreeMemory(device, block.memory, nullptr);
    }
    blocks.clear();
}

} // namespace

extern "C" void bongo_cat_vulkan_buffer_create(VkDevice device,
    VkPhysicalDevice physical, VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties, VkBuffer *buffer_out,
    VkDeviceMemory *memory_out) {
    if (!buffer_out || !memory_out) return;
    *buffer_out = VK_NULL_HANDLE;
    *memory_out = VK_NULL_HANDLE;
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS ||
        buffer == VK_NULL_HANDLE)
        return;
    VkMemoryRequirements needs = {};
    vkGetBufferMemoryRequirements(device, buffer, &needs);

    std::lock_guard<std::mutex> guard(pool_mutex);
    const uint32_t type = find_memory_type(physical, needs.memoryTypeBits,
        properties);
    if (type == UINT32_MAX) {
        vkDestroyBuffer(device, buffer, nullptr);
        return;
    }
    /* 按对齐取整就够了, **不要**按 kGranularity 取整: 一个 448 字节的 uniform 缓冲如果占掉
       64KB, 1900 个就是 121MB —— 那正是这次要消灭的那类浪费。kGranularity 只用于空闲区间的
       切分/合并, 不参与单次分配。 */
    const VkDeviceSize granule = needs.alignment > 256u ? needs.alignment : 256u;
    const VkDeviceSize wanted = align_up(needs.size, granule);
    size_t chosen = blocks.size();
    VkDeviceSize offset = 0;
    for (size_t index = 0; index < blocks.size(); ++index) {
        Block &block = blocks[index];
        if (block.type_index != type || block.properties != properties) continue;
        if (take_range(&block, wanted, needs.alignment, &offset)) {
            chosen = index;
            break;
        }
    }
    if (chosen == blocks.size()) {
        Block fresh;
        fresh.size = std::max(kBlockBytes, wanted);
        fresh.type_index = type;
        fresh.properties = properties;
        VkMemoryAllocateInfo alloc = {};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = fresh.size;
        alloc.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &alloc, nullptr, &fresh.memory) != VK_SUCCESS) {
            vkDestroyBuffer(device, buffer, nullptr);
            return;
        }
        if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            vkMapMemory(device, fresh.memory, 0, VK_WHOLE_SIZE, 0, &fresh.mapped);
        blocks.push_back(std::move(fresh));
        chosen = blocks.size() - 1;
        offset = 0;
        blocks[chosen].used = wanted;
    }
    if (vkBindBufferMemory(device, buffer, blocks[chosen].memory, offset) !=
        VK_SUCCESS) {
        return_range(&blocks[chosen], offset, wanted);
        vkDestroyBuffer(device, buffer, nullptr);
        return;
    }
    allocations[buffer] = {chosen, offset, wanted};
    blocks[chosen].live++;
    live_buffers++;
    *buffer_out = buffer;
    *memory_out = blocks[chosen].memory;
}

extern "C" void *bongo_cat_vulkan_buffer_mapped(VkBuffer buffer) {
    std::lock_guard<std::mutex> guard(pool_mutex);
    const auto found = allocations.find(buffer);
    if (found == allocations.end()) return nullptr;
    const Block &block = blocks[found->second.block];
    if (!block.mapped) return nullptr;
    return static_cast<unsigned char *>(block.mapped) + found->second.offset;
}

/* 把一整块还给驱动, 并把所有记录里比它靠后的块下标前移一位 (块数量很少, O(n) 足够)。 */
static void drop_block(VkDevice device, size_t index) {
    Block &block = blocks[index];
    if (block.mapped) vkUnmapMemory(device, block.memory);
    vkFreeMemory(device, block.memory, nullptr);
    blocks.erase(blocks.begin() + (long)index);
    for (auto &entry : allocations) {
        if (entry.second.block > index) entry.second.block--;
    }
}

extern "C" void bongo_cat_vulkan_buffer_release(VkDevice device, VkBuffer buffer) {
    if (buffer == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> guard(pool_mutex);
    const auto found = allocations.find(buffer);
    /* 重复释放: 池子已经不知道它了, 再 vkDestroyBuffer 一次会把别人的句柄干掉 */
    if (found == allocations.end()) return;
    const Allocation allocation = found->second;
    allocations.erase(found);
    size_t block_index = allocation.block;
    if (block_index < blocks.size()) {
        return_range(&blocks[block_index], allocation.offset, allocation.size);
        if (blocks[block_index].live) blocks[block_index].live--;
    }
    if (live_buffers) live_buffers--;
    vkDestroyBuffer(device, buffer, nullptr);
    /* 整块空了就立刻还给驱动: 否则 8K 贴图那块 134MB 的临时 staging 会一直挂到进程退出 */
    if (block_index < blocks.size() && blocks[block_index].live == 0)
        drop_block(device, block_index);
    else
        release_blocks_if_idle(device);
}
