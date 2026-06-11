-- NVRHI premake5 build script
-- MIT -- GPU abstraction (D3D12 + Vulkan backends; D3D11 off per the
-- 2026-06-10 stack spec). Mirrors upstream CMakeLists options; the copy
-- kept at CMakeLists.txt.reference is the reconciliation source on pulls
-- (stack-spec risk R1: wrapper drift).
--
-- Vulkan: the backend defines VULKAN_HPP_DISPATCH_LOADER_DYNAMIC itself
-- (vulkan-backend.h), so consumers need no vulkan-1.lib import library.
-- D3D12: consumers link d3d12 + dxgi + dxguid.

project "nvrhi"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "include/nvrhi/**.h",
        "src/common/**.cpp",
        "src/common/**.h",
        "src/validation/**.cpp",
        "src/validation/**.h",
        "src/d3d12/**.cpp",
        "src/d3d12/**.h",
        "src/vulkan/**.cpp",
        "src/vulkan/**.h",
    }

    includedirs {
        "include",
        "src",
        "../Vulkan-Headers/include",
        "../DirectX-Headers/include",
        "../DirectX-Headers/include/directx",
    }

    defines {
        "NVRHI_WITH_DX11=0",
        "NVRHI_WITH_DX12=1",
        "NVRHI_WITH_VULKAN=1",
        "NVRHI_WITH_AFTERMATH=0",
        "VK_USE_PLATFORM_WIN32_KHR",
        "NOMINMAX",
        "WIN32_LEAN_AND_MEAN",
        "_CRT_SECURE_NO_WARNINGS",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/bigobj" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }
