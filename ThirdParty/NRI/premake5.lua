-- NRI premake5 build script
-- MIT -- NVIDIA Render Interface: a thin, backend-agnostic GPU abstraction
-- underneath NVRHI in the Phase 1 substrate (spec:
-- docs/plans/2026-08-13-nri-phase1-substrate.md). Mirrors upstream
-- CMakeLists.txt's target_compile_definitions/target_sources shape for the
-- NRI_Shared + NRI_D3D12 + NRI_VK + NRI_NONE + NRI_Validation + NRI
-- targets, collapsed into one static-lib project (see README.md for the
-- excluded backends and the update procedure).
--
-- Backend selection mirrors CMakeLists.txt's COMPILE_DEFINITIONS loop: an
-- enabled NRI_ENABLE_* option gets "=1" appended; a disabled one is left
-- completely UNDEFINED (not "=0") -- every "#if NRI_ENABLE_*" guard in the
-- vendored sources evaluates an undefined macro as 0, and NRI's own /W4
-- build relies on exactly that (C4668 "not defined as a preprocessor
-- macro" is a /Wall-only warning, never emitted under plain /W4). This
-- project follows the same convention: only the four enabled backends
-- appear in `defines`, nothing enumerates the disabled ones.
--
-- D3D11 and WGPU are excluded at the FILE level (Step 1 never copies
-- Source/D3D11 or Source/WGPU), so NRI_ENABLE_D3D11_SUPPORT and
-- NRI_ENABLE_WGPU_SUPPORT must stay undefined -- Source/Creation/
-- Creation.cpp guards every reference to those backends' headers/functions
-- behind the matching "#if NRI_ENABLE_*_SUPPORT".
--
-- NVAPI and AMDAGS are vendor SDKs NRI's own CMakeLists.txt FetchContents
-- from GitHub (nvapi, AGS_SDK) -- neither is part of this repo's vendor
-- set (Task 1 vendored only VMA + D3D12MA), so NRI_ENABLE_NVAPI and
-- NRI_ENABLE_AMDAGS stay undefined too: defining either would compile in
-- "#include <nvapi.h>" / "#include <amd_ags.h>" guards in the D3D12
-- sources with no header to satisfy them.
--
-- Agility SDK: NRI_ENABLE_AGILITY_SDK_SUPPORT is written explicitly (=0,
-- not omitted) so Task 3 -- which turns it on -- has a single value to
-- flip rather than a new line to add.

project "NRI"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"    -- NRI's own CMakeLists.txt: target_compile_features(NRI_Shared PUBLIC cxx_std_17)
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        -- NRI.h/NRIDescs.h/NRIMacro.h/Extensions/*.h only -- Include/NRI.hlsl
        -- (a shared HLSL/C++ struct-layout header, not a shader entry
        -- point) is deliberately NOT globbed here: Visual Studio infers
        -- the FxCompile build action from the .hlsl extension and would
        -- try to compile it as a shader ("entrypoint 'main' not found"),
        -- the same reason upstream's CMakeLists.txt marks it
        -- ExcludedFromBuild=true. The file still ships on disk (Step 1's
        -- "Include/ (all)" copy) for anything that #includes it.
        "Include/**.h",
        "Source/NRIConfig.h",
        "Source/Shared/**.h",
        "Source/Shared/**.hpp",
        "Source/Shared/**.cpp",
        "Source/D3D12/**.h",
        "Source/D3D12/**.hpp",
        "Source/D3D12/**.cpp",
        "Source/VK/**.h",
        "Source/VK/**.hpp",
        "Source/VK/**.cpp",
        "Source/NONE/**.cpp",
        "Source/Validation/**.h",
        "Source/Validation/**.hpp",
        "Source/Validation/**.cpp",
        "Source/Creation/**.cpp",
    }

    includedirs {
        "Include",
        "Source",
        "Source/Shared",
        "%{IncludeDir.VulkanHeaders}",
        "%{IncludeDir.DirectXHeaders}",
        "%{IncludeDir.DirectXHeaders}/directx",
        "%{IncludeDir.VMA}",
        -- D3D12MA: BOTH include/ and src/ -- MemoryAllocatorD3D12.h does
        -- "#include "D3D12MemAlloc.cpp"" textually (never compiled as its
        -- own TU; see ThirdParty/D3D12MA/README.md).
        "%{IncludeDir.D3D12MA}",
        "%{wks.location}/ThirdParty/D3D12MA/src",
    }

    defines {
        -- Backends: D3D12 + VK + NONE + Validation. D3D11/WGPU/NVAPI/AMDAGS/
        -- NVTX left undefined (see the file-header comment above).
        "NRI_ENABLE_D3D12_SUPPORT=1",
        "NRI_ENABLE_VK_SUPPORT=1",
        "NRI_ENABLE_NONE_SUPPORT=1",
        "NRI_ENABLE_VALIDATION_SUPPORT=1",
        -- Task 3 flips this to 1 and wires the Agility SDK vendor drop.
        "NRI_ENABLE_AGILITY_SDK_SUPPORT=0",
        -- NRI_Shared's PUBLIC compile definitions (CMakeLists.txt:417-423).
        "WIN32_LEAN_AND_MEAN",
        "NOMINMAX",
        "_CRT_SECURE_NO_WARNINGS",
    }

    -- NRI's own CMakeLists.txt builds NRI_Shared (and everything linked to
    -- it) at "/W4 /WX" for the MSVC frontend (CMakeLists.txt:438-443,
    -- "$<$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>: /W4 /WX ...>"). Mirror that
    -- here rather than the workspace's default /W3 -- this is vendored
    -- code that ships /W4 /WX-clean upstream, so any warning surfaced by
    -- our own toolset is worth seeing, not silently downgraded.
    warnings "Extra"
    -- C4324 "structure was padded due to alignment specifier": upstream
    -- disables this too (CMakeLists.txt:441, "/wd4324") even in their own
    -- /W4 /WX build -- not a suppression this wrapper introduced.
    disablewarnings { "4324" }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/bigobj" }
        fatalwarnings { "All" }
        defines { "VK_USE_PLATFORM_WIN32_KHR" }   -- VK backend (CMakeLists.txt:749-751)

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        runtime "Release"
        optimize "on"
        -- NDEBUG must match every other statically-linked dependency in
        -- this workspace (see the workspace premake5.lua:271-276 note on
        -- nvrhi/ArcaneClient's DispatchLoaderDynamic layout hazard) -- NRI's
        -- own D3D12 Agility SDK path and VK dispatch tables carry the same
        -- NDEBUG-gated layout risk.
        defines { "NDEBUG" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }
