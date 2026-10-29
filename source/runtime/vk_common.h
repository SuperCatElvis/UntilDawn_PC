/* vk_common.h - Vulkan loading for the Windows build (vulkan-1.dll ships with the GPU driver).
 * Uses the Khronos headers in vk/ (Apache-2.0 OR MIT). No SDK or import library is needed. */
#ifndef VK_COMMON_H
#define VK_COMMON_H
#define VK_NO_PROTOTYPES
#include "vk/vulkan/vulkan_core.h"

/* Win32 surface (declared here so windows.h is not needed) */
typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; void *hinstance; void *hwnd; } VkWin32SurfaceCreateInfoKHR_;
typedef VkResult (VKAPI_PTR *PFN_vkCreateWin32SurfaceKHR_)(VkInstance, const VkWin32SurfaceCreateInfoKHR_ *, const VkAllocationCallbacks *, VkSurfaceKHR *);
#define VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR_ ((VkStructureType)1000009000)

__declspec(dllimport) void *WINAPI LoadLibraryA(const char *);
__declspec(dllimport) void *WINAPI GetProcAddress(void *, const char *);

#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceMemoryProperties2) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkDestroySurfaceKHR)
#define VK_DEVICE_FNS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkFlushMappedMemoryRanges) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkGetBufferDeviceAddress) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkGetImageMemoryRequirements2) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) \
    X(vkDestroyPipeline) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCmdBindPipeline) X(vkCmdDispatch) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdBindIndexBuffer) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) \
    X(vkCmdBlitImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdBeginRendering) X(vkCmdEndRendering) \
    X(vkCmdPushDescriptorSetKHR) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) X(vkCmdCopyBuffer) X(vkCmdClearAttachments) X(vkCmdSetBlendConstants) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkGetMemoryHostPointerPropertiesEXT) \
    X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkGetQueryPoolResults) X(vkCreatePipelineCache) X(vkGetPipelineCacheData)

#define VK_DECL(f) static PFN_##f f;
VK_INSTANCE_FNS(VK_DECL)
VK_DEVICE_FNS(VK_DECL)
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static PFN_vkCreateInstance vkCreateInstance;
static PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;
static PFN_vkCreateWin32SurfaceKHR_ vkCreateWin32SurfaceKHR;

static int vk_load_global(void) {
    void *dll = LoadLibraryA("vulkan-1.dll");
    if (!dll) return 0;
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(dll, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) return 0;
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(0, "vkCreateInstance");
    vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(0, "vkEnumerateInstanceVersion");
    return vkCreateInstance != 0;
}
static void vk_load_instance(VkInstance inst) {
#define VK_LI(f) f = (PFN_##f)vkGetInstanceProcAddr(inst, #f);
    VK_INSTANCE_FNS(VK_LI)
    vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR_)vkGetInstanceProcAddr(inst, "vkCreateWin32SurfaceKHR");
}
static void vk_load_device(VkDevice dev) {
#define VK_LD(f) f = (PFN_##f)vkGetDeviceProcAddr(dev, #f);
    VK_DEVICE_FNS(VK_LD)
}
static int vk_has_ext(VkPhysicalDevice pd, const char *name) {
    static VkExtensionProperties props[512];
    u32 n = 512;
    if (vkEnumerateDeviceExtensionProperties(pd, 0, &n, props) < 0) return 0;
    for (u32 i = 0; i < n; i++) if (str_eq(props[i].extensionName, name)) return 1;
    return 0;
}
static u32 vk_find_mem(VkPhysicalDevice pd, u32 bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0xFFFFFFFFu;
}
#endif
