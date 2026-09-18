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
    vk->validation = validation_requested() && layer_available(k_validation_layer);
    if (vk->validation) extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (vk->validation && !layer_available(k_validation_layer)) vk->validation = false;

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
            "Vulkan 实例创建失败 (VkResult=%d)", (int)result);
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
    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo create = {0};
    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.enabledExtensionCount = 1;
    create.ppEnabledExtensionNames = extensions;
    VkResult result = vkCreateDevice(vk->physical, &create, NULL, &vk->device);
    if (result != VK_SUCCESS) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Vulkan 逻辑设备创建失败 (VkResult=%d)", (int)result);
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

bool bongo_cat_vk_compiled(void) { return true; }

static BongoCatVk *current_context;

BongoCatVk *bongo_cat_vk_current(void) { return current_context; }

bool bongo_cat_vk_probe(BongoCatError *error) {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    BongoCatVk probe = {0};
    BongoCatError local = {0};
    BongoCatError *failure = error ? error : &local;
    bool usable = bongo_cat_vk_create_instance(&probe, failure) &&
        bongo_cat_vk_create_device(&probe, failure);
    bongo_cat_vk_destroy_device(&probe);
    cached = usable ? 1 : 0;
    return usable;
}

BongoCatVk *bongo_cat_vk_create(SDL_Window *window, BongoCatError *error) {
    if (!window) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT, "Vulkan 后端缺少窗口");
        return NULL;
    }
    BongoCatVk *vk = calloc(1, sizeof(*vk));
    if (!vk) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY, "Vulkan 后端状态分配失败");
        return NULL;
    }
    vk->window = window;
    if (!bongo_cat_vk_create_instance(vk, error)) {
        bongo_cat_vk_destroy(vk);
        return NULL;
    }
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    if (width <= 0 || height <= 0) { width = 1; height = 1; }
    if (!SDL_Vulkan_CreateSurface(window, vk->instance, NULL, &vk->surface)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 表面创建失败: %s",
            SDL_GetError());
        bongo_cat_vk_destroy(vk);
        return NULL;
    }
    if (!bongo_cat_vk_create_device(vk, error)) {
        bongo_cat_vk_destroy(vk);
        return NULL;
    }
    /* 顺序有依赖: 命令池 (帧同步) 要早于纹理上传, 管线要晚于渲染通道。 */
    if (!bongo_cat_vk_create_frame_sync(vk, error) ||
        !bongo_cat_vk_create_texture_resources(vk, error) ||
        !bongo_cat_vk_create_swapchain(vk, width, height, error) ||
        !bongo_cat_vk_create_draw_resources(vk, error)) {
        bongo_cat_vk_destroy(vk);
        return NULL;
    }
    vk->info.width = (int)vk->extent.width;
    vk->info.height = (int)vk->extent.height;
    current_context = vk;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[vulkan] 设备就绪 device=%s driver=%s api=%s swapchain=%ux%u format=%d "
        "present_mode=%d validation=%d",
        vk->info.device, vk->info.driver, vk->info.api,
        (unsigned)vk->extent.width, (unsigned)vk->extent.height,
        (int)vk->format, (int)vk->present_mode, vk->validation ? 1 : 0);
    return vk;
}

void bongo_cat_vk_destroy(BongoCatVk *vk) {
    if (!vk) return;
    if (current_context == vk) current_context = NULL;
    if (vk->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vk->device);
        /* 管线先于渲染通道销毁, 纹理后于使用它们的帧同步销毁 */
        bongo_cat_vk_destroy_draw_resources(vk);
        bongo_cat_vk_destroy_swapchain(vk);
        bongo_cat_vk_destroy_texture_resources(vk);
        bongo_cat_vk_destroy_frame_sync(vk);
    }
    bongo_cat_vk_destroy_device(vk);
    free(vk);
}

bool bongo_cat_vk_info(const BongoCatVk *vk, BongoCatVkInfo *info) {
    if (!vk || !info) return false;
    *info = vk->info;
    return true;
}

void bongo_cat_vk_set_clear_color(BongoCatVk *vk, float red, float green,
    float blue, float alpha) {
    if (!vk) return;
    vk->clear.float32[0] = red;
    vk->clear.float32[1] = green;
    vk->clear.float32[2] = blue;
    vk->clear.float32[3] = alpha;
}

#endif /* BONGO_CAT_HAS_VULKAN */
