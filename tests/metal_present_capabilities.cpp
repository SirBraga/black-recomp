// Verify that the configured MoltenVK runtime supports GPU-resident Metal export.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <dlfcn.h>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
bool hasExtension(const std::vector<VkExtensionProperties> &extensions, const char *name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto &extension)
                       { return std::strcmp(extension.extensionName, name) == 0; });
}
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: metal_present_capabilities /path/to/libMoltenVK.dylib\n";
        return 2;
    }

    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library)
    {
        std::cerr << dlerror() << '\n';
        return 2;
    }
    const auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
    const auto enumerateInstanceExtensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        getProc(nullptr, "vkEnumerateInstanceExtensionProperties"));
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(getProc(nullptr, "vkCreateInstance"));
    uint32_t extensionCount = 0;
    if (enumerateInstanceExtensions(nullptr, &extensionCount, nullptr) != VK_SUCCESS)
        return 3;
    std::vector<VkExtensionProperties> instanceExtensions(extensionCount);
    if (enumerateInstanceExtensions(nullptr, &extensionCount, instanceExtensions.data()) != VK_SUCCESS)
        return 3;

    const char *portability = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Black GPU presentation capability probe";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &app;
    if (hasExtension(instanceExtensions, portability))
    {
        createInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        createInfo.enabledExtensionCount = 1;
        createInfo.ppEnabledExtensionNames = &portability;
    }

    VkInstance instance = VK_NULL_HANDLE;
    if (createInstance(&createInfo, nullptr, &instance) != VK_SUCCESS)
        return 4;
    const auto getInstanceProc = [&](const char *name)
    {
        return getProc(instance, name);
    };
    const auto enumerateDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(getInstanceProc("vkEnumeratePhysicalDevices"));
    const auto enumerateDeviceExtensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(getInstanceProc("vkEnumerateDeviceExtensionProperties"));
    const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(getInstanceProc("vkGetPhysicalDeviceProperties"));
    uint32_t deviceCount = 0;
    if (enumerateDevices(instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0)
        return 5;

    bool capableDeviceFound = false;
    std::vector<VkPhysicalDevice> devices(deviceCount);
    enumerateDevices(instance, &deviceCount, devices.data());
    for (VkPhysicalDevice device : devices)
    {
        uint32_t count = 0;
        if (enumerateDeviceExtensions(device, nullptr, &count, nullptr) != VK_SUCCESS)
            continue;
        std::vector<VkExtensionProperties> deviceExtensions(count);
        if (enumerateDeviceExtensions(device, nullptr, &count, deviceExtensions.data()) != VK_SUCCESS)
            continue;

        VkPhysicalDeviceProperties properties{};
        getProperties(device, &properties);
        const bool hasMetalObjects = hasExtension(deviceExtensions, "VK_EXT_metal_objects");
        std::cout << properties.deviceName << ": metal_objects=" << hasMetalObjects << '\n';
        capableDeviceFound = capableDeviceFound || hasMetalObjects;
    }

    reinterpret_cast<PFN_vkDestroyInstance>(getInstanceProc("vkDestroyInstance"))(instance, nullptr);
    std::cout << (capableDeviceFound ? "GPU_PRESENT_CAPABILITIES=PASS\n" : "GPU_PRESENT_CAPABILITIES=FAIL\n");
    return capableDeviceFound ? 0 : 1;
}
