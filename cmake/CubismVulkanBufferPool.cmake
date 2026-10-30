# Cubism SDK 的 Vulkan 缓冲分配补丁。
#
# 背景: SDK 的 CubismBufferVulkan::CreateBuffer 是"一个缓冲一次 vkAllocateMemory"。一个
# 313 drawable 的模型要给 staging/vertex/index/uniform 各建一套, 再乘交换链图像数 (3),
# 合计近两千次独立分配。实测 (RTX 4060, 234x234 窗口):
#
#     21  drawable 的内置模型: 建缓冲 73ms,  提交内存 +87MB
#     313 drawable 的用户模型: 建缓冲 1.1s, 提交内存 +500MB   (≈1.6MB/drawable)
#
# 500MB / 1878 次 ≈ 266KB/次, 而实际负载只有几 KB —— 差异全在被驱动按块取整的"每次分配"上。
#
# 补丁只做一件事: 把 CubismBufferVulkan 的建/映射/销毁转到我们自己的分配池
# (src/live2d/cubism_vulkan_buffer_pool.cpp)。SDK 的类定义、调用点、绘制路径一个字节都不改,
# vendor 目录保持原样 (配置期生成一份替换源, 与 CubismShaderOptimize 同一套做法)。
#
# 每个锚点都断言"必须出现且只出现一次": SDK 升级后对不上就在配置阶段报错, 不会悄悄生成一份
# 没打上补丁的源。

function(bongo_cat_replace_cubism_buffer_text variable needle replacement label)
  set(text "${${variable}}")
  string(FIND "${text}" "${needle}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Cubism 5 r.5 Vulkan buffer patch mismatch: ${label}")
  endif()
  string(LENGTH "${needle}" needle_length)
  math(EXPR tail_start "${position} + ${needle_length}")
  string(SUBSTRING "${text}" 0 ${position} head)
  string(SUBSTRING "${text}" ${tail_start} -1 tail)
  string(FIND "${tail}" "${needle}" duplicate)
  if(NOT duplicate EQUAL -1)
    message(FATAL_ERROR "Cubism 5 r.5 Vulkan buffer patch is ambiguous: ${label}")
  endif()
  set(${variable} "${head}${replacement}${tail}" PARENT_SCOPE)
endfunction()

function(bongo_cat_pool_cubism_vulkan_buffers target)
  if(NOT BONGO_CAT_ENABLE_VULKAN)
    return()
  endif()
  set(source_path
    "${CUBISM_FRAMEWORK_PATH}/src/Rendering/Vulkan/CubismClass_Vulkan.cpp")
  set(output_dir "${CMAKE_BINARY_DIR}/generated/cubism")
  set(output_source "${output_dir}/CubismClass_Vulkan.cpp")
  file(READ "${source_path}" source)
  string(REPLACE "\r\n" "\n" source "${source}")

  # 1) 建缓冲: 原来的"建 vkBuffer + 查需求 + 申请独立内存 + 绑定"整段交给池
  bongo_cat_replace_cubism_buffer_text(source
    [=[    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
    {
        CubismLogError("failed to create buffer!");
    }

    VkMemoryRequirements memRequirements{};
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physicalDevice, memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
    {
        CubismLogError("failed to allocate buffer memory!");
    }

    vkBindBufferMemory(device, buffer, memory, 0);]=]
    [=[    /* bongo-cat 补丁: 从宿主分配池里切一块, 见 src/live2d/cubism_vulkan_buffer_pool.cpp */
    bongo_cat_vulkan_buffer_create(device, physicalDevice, size, usage, properties,
                                   &buffer, &memory);]=]
    "create buffer allocation")

  # 2) 映射: 池块已经持久映射, 这里只算这个缓冲在块内的地址
  bongo_cat_replace_cubism_buffer_text(source
    [=[    vkMapMemory(device, memory, 0, size, 0, &mapped);]=]
    [=[    (void)device;
    (void)size;
    mapped = bongo_cat_vulkan_buffer_mapped(buffer);]=]
    "map buffer")

  # 3) 取消映射: 池块在进程内保持映射, 逐缓冲解映射会让同块里的其它缓冲失效
  bongo_cat_replace_cubism_buffer_text(source
    [=[    vkUnmapMemory(device, memory);]=]
    [=[    (void)device;]=]
    "unmap buffer")

  # 4) 销毁: 只销毁缓冲, 内存还给池 (整块在最后一个缓冲释放时归还)
  bongo_cat_replace_cubism_buffer_text(source
    [=[    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);]=]
    [=[    bongo_cat_vulkan_buffer_release(device, buffer);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    mapped = nullptr;]=]
    "destroy buffer")

  # 5) 声明池接口的头
  bongo_cat_replace_cubism_buffer_text(source
    [=[#include "CubismClass_Vulkan.hpp"]=]
    [=[#include "CubismClass_Vulkan.hpp"
#include "cubism_vulkan_buffer_pool.h"]=]
    "pool header include")

  file(MAKE_DIRECTORY "${output_dir}")
  file(WRITE "${output_source}" "${source}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source_path}")

  get_target_property(framework_sources ${target} SOURCES)
  list(REMOVE_ITEM framework_sources "${source_path}")
  set_property(TARGET ${target} PROPERTY SOURCES "${framework_sources}")
  target_sources(${target} PRIVATE
    "${output_source}"
    "${CMAKE_SOURCE_DIR}/src/live2d/cubism_vulkan_buffer_pool.cpp")
  target_include_directories(${target} PRIVATE
    "${CMAKE_SOURCE_DIR}/src/live2d"
    # 替换源放在 generated/ 下, 而它 include 的 SDK 头是相对原本目录的
    "${CUBISM_FRAMEWORK_PATH}/src/Rendering/Vulkan")
  message(STATUS "Cubism Vulkan 缓冲分配改为池式 (生成源: ${output_source})")
endfunction()
