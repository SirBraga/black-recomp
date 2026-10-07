// Probe only; does not render or modify system graphics configuration.
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <iostream>
#include <vector>
#include <cstring>
int main(int argc,char**argv) {
 if(argc!=2){std::cerr<<"usage: probe /path/to/libMoltenVK.dylib\n";return 2;}
 void* lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){std::cerr<<dlerror()<<"\n";return 2;}
 auto proc=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib,"vkGetInstanceProcAddr"));
 auto create=reinterpret_cast<PFN_vkCreateInstance>(proc(nullptr,"vkCreateInstance"));
 VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};application.pApplicationName="Black GS capability probe";application.apiVersion=VK_API_VERSION_1_2;
 const char* extension="VK_KHR_portability_enumeration";VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&application;auto enumerateExtensions=reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(proc(nullptr,"vkEnumerateInstanceExtensionProperties"));
 uint32_t extensionCount=0;enumerateExtensions(nullptr,&extensionCount,nullptr);std::vector<VkExtensionProperties> available(extensionCount);enumerateExtensions(nullptr,&extensionCount,available.data());
 for(const auto& e:available)if(std::strcmp(e.extensionName,extension)==0){ci.flags=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;ci.enabledExtensionCount=1;ci.ppEnabledExtensionNames=&extension;}

 VkInstance instance{};auto result=create(&ci,nullptr,&instance);if(result){std::cerr<<"createInstance="<<result<<"\n";return 2;}
 auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(proc(instance,"vkEnumeratePhysicalDevices"));
 auto features=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(proc(instance,"vkGetPhysicalDeviceFeatures2"));
 auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(proc(instance,"vkGetPhysicalDeviceProperties2"));
 uint32_t count=0;enumerate(instance,&count,nullptr);std::vector<VkPhysicalDevice> devices(count);enumerate(instance,&count,devices.data());
 for(auto device:devices){
  VkPhysicalDeviceSubgroupSizeControlFeatures subgroupFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
  VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};v12.pNext=&subgroupFeatures;
  VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};v11.pNext=&v12;
  VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f.pNext=&v11;features(device,&f);
  VkPhysicalDeviceSubgroupSizeControlProperties subSize{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
  VkPhysicalDeviceVulkan11Properties vp11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};vp11.pNext=&subSize;
  VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p.pNext=&vp11;properties(device,&p);
  std::cout<<"device="<<p.properties.deviceName<<"\n";
  auto show=[](const char* name,VkBool32 supported){std::cout<<name<<"="<<supported<<"\n";};
  show("descriptorIndexing",v12.descriptorIndexing);show("timelineSemaphore",v12.timelineSemaphore);show("bufferDeviceAddress",v12.bufferDeviceAddress);show("storageBuffer8BitAccess",v12.storageBuffer8BitAccess);show("storageBuffer16BitAccess",v11.storageBuffer16BitAccess);show("shaderInt16",f.features.shaderInt16);show("scalarBlockLayout",v12.scalarBlockLayout);show("subgroupSizeControl",subgroupFeatures.subgroupSizeControl);show("computeFullSubgroups",subgroupFeatures.computeFullSubgroups);
  std::cout<<"subgroupSize="<<vp11.subgroupSize<<" operations="<<vp11.subgroupSupportedOperations<<" min="<<subSize.minSubgroupSize<<" max="<<subSize.maxSubgroupSize<<"\nsharedMemory="<<p.properties.limits.maxComputeSharedMemorySize<<"\n";
  VkSubgroupFeatureFlags required=VK_SUBGROUP_FEATURE_ARITHMETIC_BIT|VK_SUBGROUP_FEATURE_SHUFFLE_BIT|VK_SUBGROUP_FEATURE_VOTE_BIT|VK_SUBGROUP_FEATURE_BALLOT_BIT|VK_SUBGROUP_FEATURE_BASIC_BIT;
  bool basic=v12.descriptorIndexing&&v12.timelineSemaphore&&v12.bufferDeviceAddress&&v12.storageBuffer8BitAccess&&v11.storageBuffer16BitAccess&&f.features.shaderInt16&&v12.scalarBlockLayout&&(vp11.subgroupSupportedOperations&required)==required&&p.properties.limits.maxComputeSharedMemorySize>=32768;
  std::cout<<"parallel_gs_base_requirements="<<(basic?"PASS (subgroup dispatch/build still need validation)":"FAIL")<<"\n";
 }
 auto destroy=reinterpret_cast<PFN_vkDestroyInstance>(proc(instance,"vkDestroyInstance"));destroy(instance,nullptr);return devices.empty()?1:0;
}
