// Query Vulkan cache identities without creating a device or starting the game.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    void *loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader) return 1;
    PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");
    if (!get) return 1;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)get(VK_NULL_HANDLE, "vkCreateInstance");
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    const VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    if (!create || create(&info, NULL, &instance) != VK_SUCCESS) return 1;

    PFN_vkEnumeratePhysicalDevices list = (PFN_vkEnumeratePhysicalDevices)get(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties2 properties = (PFN_vkGetPhysicalDeviceProperties2)get(instance, "vkGetPhysicalDeviceProperties2");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)get(instance, "vkDestroyInstance");
    if (!destroy) return 1;
    uint32_t count = 0;
    VkPhysicalDevice *devices = NULL;
    int status = 1;
    if (!list || !properties || list(instance, &count, NULL) != VK_SUCCESS || count == 0) goto done;
    devices = calloc(count, sizeof(*devices));
    if (!devices || list(instance, &count, devices) != VK_SUCCESS) goto done;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
        properties(devices[i], &props);
        if (driver.driverID != VK_DRIVER_ID_MESA_TURNIP) continue;
        printf("%u %u %u %u ", props.properties.vendorID, props.properties.deviceID, (unsigned)driver.driverID,
               props.properties.driverVersion);
        for (size_t j = 0; j < VK_UUID_SIZE; ++j) printf("%02x", (unsigned)props.properties.pipelineCacheUUID[j]);
        puts("");
    }
    status = 0;
done:
    free(devices);
    destroy(instance, NULL);
    dlclose(loader);
    return status;
}
