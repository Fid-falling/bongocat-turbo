#include "vk_internal.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BONGO_CAT_HAS_VULKAN

static const char *k_validation_layer = "VK_LAYER_KHRONOS_validation";

static bool validation_requested(void) {
    const char *value = SDL_getenv("BONGO_CAT_VULKAN_VALIDATION");
    return value && value[0] && strcmp(value, "0") != 0;
}

static bool layer_available(const char *name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS) return false;
    VkLayerProperties *layers = calloc(count ? count : 1, sizeof(*layers));
    if (!layers) return false;
    bool found = false;
    if (vkEnumerateInstanceLayerProperties(&count, layers) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count && !found; ++i)
            found = strcmp(layers[i].layerName, name) == 0;
    }
    free(layers);
    return found;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user) {
    (void)types;
    (void)user;
    const char *text = data && data->pMessage ? data->pMessage : "unknown";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "[vulkan] %s", text);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "[vulkan] %s", text);
    else
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[vulkan] %s", text);
    return VK_FALSE;
}

static void api_version_text(uint32_t version, char *text, size_t capacity) {
    snprintf(text, capacity, "%u.%u.%u", VK_API_VERSION_MAJOR(version),
        VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
}

static void driver_text(VkPhysicalDeviceProperties *properties, char *text, size_t capacity) {
    /* VkPhysicalDeviceProperties::driverID 只有 1.1 头里才无条件提供, 这里只按
       PCI 厂商号给出人类可读的名字, 足够日志排查用。 */
    switch (properties->vendorID) {
    case 0x10DE: snprintf(text, capacity, "NVIDIA"); return;
    case 0x1002: snprintf(text, capacity, "AMD"); return;
    case 0x8086: snprintf(text, capacity, "Intel"); return;
    case 0x5143: snprintf(text, capacity, "Qualcomm"); return;
    case 0x1010: snprintf(text, capacity, "Imagination"); return;
    case 0x13B5: snprintf(text, capacity, "ARM"); return;
    default: snprintf(text, capacity, "vendorID=0x%04X", properties->vendorID); return;
    }
}

static bool device_extension_supported(VkPhysicalDevice device, const char *name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL) != VK_SUCCESS)
        return false;
    VkExtensionProperties *extensions = calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) return false;
    bool found = false;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, extensions) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count && !found; ++i)
            found = strcmp(extensions[i].extensionName, name) == 0;
    }
    free(extensions);
    return found;
}

static bool find_queue_family(VkPhysicalDevice device, VkSurfaceKHR surface,
    uint32_t *family) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);
    if (!count) return false;
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) return false;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families);
    bool found = false;
    for (uint32_t i = 0; i < count && !found; ++i) {
        if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (surface == VK_NULL_HANDLE) {
            *family = i;
            found = true;
            break;
        }
        VkBool32 present = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present) == VK_SUCCESS &&
            present) {
            *family = i;
            found = true;
        }
    }
    free(families);
    return found;
}

static int device_score(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(device, &properties);
    int score = 0;
    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
    else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 500;
    else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) score += 100;
    score += (int)(properties.limits.maxImageDimension2D / 1024);
    return score;
}

bool bongo_cat_vk_create_instance(BongoCatVk *vk, BongoCatError *error) {
    uint32_t sdl_count = 0;
    const char *const *sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
    const char *extensions[8] = {0};
    uint32_t extension_count = 0;
    if (sdl_extensions) {
        for (uint32_t i = 0; i < sdl_count && extension_count < 6; ++i)
            extensions[extension_count++] = sdl_extensions[i];
    }
    /*
     * 校验层: BONGO_CAT_VULKAN_VALIDATION=1 才会请求它, 而且必须真的装了那一层才启得
     * 起来 (装 Vulkan SDK 后它的 Bin 目录要在 PATH 上)。启不起来时必须留一条日志 ——
     * 用户照着 --help 打开开关却什么都没变, 是最难查的一类"诊断设施自己没生效"。
     */
    const bool want_validation = validation_requested();
    vk->validation = want_validation && layer_available(k_validation_layer);
    if (want_validation && !vk->validation)
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] BONGO_CAT_VULKAN_VALIDATION 已设置, 但实例上没有 %s: "
            "本次不启用校验层 (装 Vulkan SDK 并让它在 PATH 上之后重试)",
            k_validation_layer);
    if (vk->validation) extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

    VkApplicationInfo application = {0};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = BONGO_CAT_NAME;
    application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application.pEngineName = BONGO_CAT_NAME;
    application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    application.apiVersion = VK_API_VERSION_1_0;

    VkDebugUtilsMessengerCreateInfoEXT debug = {0};
    debug.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = debug_message;

    VkInstanceCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &application;
    create.enabledExtensionCount = extension_count;
    create.ppEnabledExtensionNames = extension_count ? extensions : NULL;
    if (vk->validation) {
        create.enabledLayerCount = 1;
        create.ppEnabledLayerNames = &k_validation_layer;
        create.pNext = &debug;
    }
    VkResult result = vkCreateInstance(&create, NULL, &vk->instance);
    if (result != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Vulkan 实例创建失败 (VkResult=%d %s)", (int)result,
            bongo_cat_vk_result_name(result));
        return false;
    }
    if (vk->validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(vk->instance,
                "vkCreateDebugUtilsMessengerEXT");
        if (create_messenger)
            create_messenger(vk->instance, &debug, NULL, &vk->debug_messenger);
    }
    return true;
}

bool bongo_cat_vk_create_device(BongoCatVk *vk, BongoCatError *error) {
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(vk->instance, &count, NULL) != VK_SUCCESS || !count) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "系统里没有可用的 Vulkan 设备");
        return false;
    }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY, "Vulkan 设备列表分配失败");
        return false;
    }
    if (vkEnumeratePhysicalDevices(vk->instance, &count, devices) != VK_SUCCESS) {
        free(devices);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 设备枚举失败");
        return false;
    }
    int best_score = -1;
    uint32_t best_family = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t family = 0;
        if (!find_queue_family(devices[i], vk->surface, &family)) continue;
        if (!device_extension_supported(devices[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        int score = device_score(devices[i]);
        if (score > best_score) {
            best_score = score;
            vk->physical = devices[i];
            best_family = family;
        }
    }
    free(devices);
    if (vk->physical == VK_NULL_HANDLE) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "没有同时支持图形队列与窗口呈现的 Vulkan 设备");
        return false;
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(vk->physical, &properties);
    snprintf(vk->info.device, sizeof(vk->info.device), "%s", properties.deviceName);
    driver_text(&properties, vk->info.driver, sizeof(vk->info.driver));
    api_version_text(properties.apiVersion, vk->info.api, sizeof(vk->info.api));

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {0};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = best_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    /*
     * Cubism 自带的 Vulkan 渲染器需要这些设备特性 (校验层点名):
     *   - VK_EXT_extended_dynamic_state: 管线用 VK_DYNAMIC_STATE_CULL_MODE 等
     *   - dynamicRendering (1.3): 它的图形管线 pNext/ renderPass = NULL
     *   - samplerAnisotropy: 它的采样器 anisotropyEnable = VK_TRUE
     * 不启用的话建出来的管线/采样器都是无效对象, 绘制时直接崩。
     * 全部按设备实际支持情况启用, 不支持就保持关闭, 不影响我们自己的后端。
     */
    const bool extended_dynamic_state =
        device_extension_supported(vk->physical, "VK_EXT_extended_dynamic_state");
    const char *extensions[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, NULL};
    uint32_t extension_count = 1;
    if (extended_dynamic_state)
        extensions[extension_count++] = "VK_EXT_extended_dynamic_state";
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extended = {0};
    extended.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
    VkPhysicalDeviceVulkan13Features features13 = {0};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.pNext = extended_dynamic_state ? &extended : NULL;
    VkPhysicalDeviceFeatures2 features = {0};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(vk->physical, &features);
    if (properties.apiVersion < VK_API_VERSION_1_3) features13.dynamicRendering = VK_FALSE;
    if (!extended_dynamic_state) extended.extendedDynamicState = VK_FALSE;
    VkDeviceCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create.pNext = &features;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.enabledExtensionCount = extension_count;
    create.ppEnabledExtensionNames = extensions;
    VkResult result = vkCreateDevice(vk->physical, &create, NULL, &vk->device);
    if (result != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Vulkan 逻辑设备创建失败 (VkResult=%d %s)", (int)result,
            bongo_cat_vk_result_name(result));
        return false;
    }
    vk->queue_family = best_family;
    vkGetDeviceQueue(vk->device, best_family, 0, &vk->queue);
    return true;
}

void bongo_cat_vk_destroy_device(BongoCatVk *vk) {
    if (!vk) return;
    if (vk->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vk->device);
        if (vk->command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(vk->device, vk->command_pool, NULL);
        vkDestroyDevice(vk->device, NULL);
        vk->device = VK_NULL_HANDLE;
        vk->command_pool = VK_NULL_HANDLE;
    }
    if (vk->surface != VK_NULL_HANDLE && vk->instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(vk->instance, vk->surface, NULL);
        vk->surface = VK_NULL_HANDLE;
    }
    if (vk->debug_messenger != VK_NULL_HANDLE) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(vk->instance,
                "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(vk->instance, vk->debug_messenger, NULL);
        vk->debug_messenger = VK_NULL_HANDLE;
    }
    if (vk->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(vk->instance, NULL);
        vk->instance = VK_NULL_HANDLE;
    }
}

/* bongo_cat_vk_compiled / _probe / _current / _create* / _destroy / _info 以及全部
   查询接口都在 vk_context.c —— 本文件只负责 Instance 与 Device 的创建销毁。 */

#endif /* BONGO_CAT_HAS_VULKAN */
