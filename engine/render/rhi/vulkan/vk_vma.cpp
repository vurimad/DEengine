// The VMA implementation, compiled once (VMA_IMPLEMENTATION and the function-loading options are
// set for this file in CMakeLists.txt). Vulkan functions come from volk: vmaImportVulkanFunctionsFromVolk.
#include <volk.h>
// volk first: it declares the Vulkan prototypes VMA needs.
#include <vk_mem_alloc.h>
