// Vulkan-Hpp default dynamic-dispatcher storage. NVRHI's static-lib build
// (no NVRHI_SHARED_LIBRARY_BUILD) follows the standard Vulkan-Hpp contract:
// the application defines this storage in exactly one TU. ArcaneTests is
// the application for M0; in M1 this TU moves into Arcane.dll's Render
// module, which becomes the one-engine-instance-per-process owner.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VK_USE_PLATFORM_WIN32_KHR
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <vulkan/vulkan.hpp>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
