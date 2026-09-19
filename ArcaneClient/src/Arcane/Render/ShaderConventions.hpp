#pragma once

// Shader toolchain conventions -- THE single source of truth shared by the
// runtime compile service (ShaderCompiler) and referenced by the offline AOT
// script (Arcane/data/shaders/compile-shaders.bat; it cannot include this header,
// so its lines carry a pointer back here). The SPIR-V register shifts are
// t=0, s=128, b=256, u=384, and they MUST match what the render path binds
// (Nri/NriDevice's VKBindingOffsets) -- if either side changes, change it
// HERE first and fan out.
//
// THE SHIFT IS PER (TYPE, SPACE), NOT GLOBAL PER TYPE. dxc's -fvk-*-shift
// applies only to the exact register space it names; NRI's own binding-
// offset addition (Source/VK/PipelineLayoutVK.hpp's `bindingOffsets` array)
// is NOT space-conditional -- it adds the same per-type offset to a range
// regardless of which space the range's descriptor set declares. So every
// HLSL source that puts a `b`/`t`/`s`/`u` register in a NON-space0 space
// needs its own shift entry here (and in compile-shaders.bat's SPIRV_FLAGS)
// for that (type, space) pair, or the SPIR-V binding dxc assigns and the
// binding NRI writes descriptors to will disagree. mesh.hlsl (Task 8/10) is
// the first source to do this: b1 moved to space1, and the bindless
// material array sits at t0/space2 -- see the two extra shift pairs below.
// A space that stays unlisted defaults to shift 0 (dxc's own default),
// which happens to already be correct for `t` (whose space0 shift is also
// 0), but is stated explicitly here rather than relied on by coincidence.

#include <cstddef>

namespace Arcane
{
    // Entry-point + profile conventions (a shader loader derives entry names
    // from the artifact stem suffix: _vs -> vs_main, _ps -> ps_main,
    // _cs -> cs_main).
    inline constexpr const char* kVsEntry = "vs_main";
    inline constexpr const char* kPsEntry = "ps_main";
    inline constexpr const char* kCsEntry = "cs_main";
    inline constexpr const char* kVsProfile = "vs_6_5";
    inline constexpr const char* kPsProfile = "ps_6_5";
    inline constexpr const char* kCsProfile = "cs_6_5";

    // Extra dxc argv for the SPIR-V target (the DXIL target adds nothing).
    // Mirrors compile-shaders.bat's SPIRV_FLAGS exactly: every HLSL source
    // dual-compiles via `#if SPIRV` push-constant/cbuffer blocks, and the
    // -fvk-*-shift values are the register shifts named above.
    inline constexpr const char* kSpirvArgs[] = {
        "-spirv",
        "-D", "SPIRV=1",
        "-fvk-t-shift", "0",   "0",
        "-fvk-s-shift", "128", "0",
        "-fvk-b-shift", "256", "0",
        "-fvk-u-shift", "384", "0",
        // mesh.hlsl only (Task 8/10): b1 moved to space1, the bindless
        // material array to t0/space2 -- see this file's header comment for
        // why each (type, space) pair needs its own entry.
        "-fvk-b-shift", "256", "1",
        "-fvk-t-shift", "0",   "2",
        // mesh.hlsl (F3): the instance and visible-index SRVs at t0/t1 in space1
        "-fvk-t-shift", "0",   "1",
    };
    inline constexpr std::size_t kSpirvArgCount = sizeof(kSpirvArgs) / sizeof(kSpirvArgs[0]);
}
