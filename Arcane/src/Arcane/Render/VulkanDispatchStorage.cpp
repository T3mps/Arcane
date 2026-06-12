// Vulkan-Hpp default dynamic-dispatcher storage. NVRHI's static-lib build
// (no NVRHI_SHARED_LIBRARY_BUILD) follows the standard Vulkan-Hpp contract:
// each module that statically links vulkan.hpp code defines this storage in
// exactly one TU. Arcane.dll is that module -- the one-engine-instance-per-
// process owner (moved here from ArcaneTests in M1, per the M0 plan).
// VK_USE_PLATFORM_WIN32_KHR, NOMINMAX, WIN32_LEAN_AND_MEAN are project-wide
// defines on Arcane.dll -- no local redefinition needed.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
