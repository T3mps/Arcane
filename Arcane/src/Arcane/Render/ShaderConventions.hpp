#pragma once

// Shader toolchain conventions -- THE single source of truth shared by the
// runtime compile service (ShaderCompiler) and referenced by the offline AOT
// script (Arcane/shaders/compile-shaders.bat; it cannot include this header,
// so its lines carry a pointer back here). The SPIR-V register shifts MUST
// match nvrhi::VulkanBindingOffsets defaults (t=0, s=128, b=256, u=384;
// DeviceVulkan does not override them) -- if either side changes, change it
// HERE first and fan out.

#include <cstddef>

namespace Arcane
{
    // Entry-point + profile conventions (ShaderLibrary derives entry names from
    // the artifact stem suffix: _vs -> vs_main, _ps -> ps_main, _cs -> cs_main).
    inline constexpr const char* kVsEntry = "vs_main";
    inline constexpr const char* kPsEntry = "ps_main";
    inline constexpr const char* kCsEntry = "cs_main";
    inline constexpr const char* kVsProfile = "vs_6_5";
    inline constexpr const char* kPsProfile = "ps_6_5";
    inline constexpr const char* kCsProfile = "cs_6_5";

    // Extra dxc argv for the SPIR-V target (the DXIL target adds nothing).
    // Mirrors compile-shaders.bat's SPIRV_FLAGS exactly: every HLSL source
    // dual-compiles via `#if SPIRV` push-constant/cbuffer blocks, and the
    // -fvk-*-shift values are nvrhi::VulkanBindingOffsets.
    inline constexpr const char* kSpirvArgs[] = {
        "-spirv",
        "-D", "SPIRV=1",
        "-fvk-t-shift", "0",   "0",
        "-fvk-s-shift", "128", "0",
        "-fvk-b-shift", "256", "0",
        "-fvk-u-shift", "384", "0",
    };
    inline constexpr std::size_t kSpirvArgCount = sizeof(kSpirvArgs) / sizeof(kSpirvArgs[0]);
}
