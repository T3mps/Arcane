-- Dear ImGui premake5 build script (Arcane workspace consumer).
-- MIT -- immediate-mode UI. Compiles imgui core + the SDL3 platform
-- backend; the NVRHI renderer backend is first-party engine code
-- (Arcane/Arcane/src/Arcane/ImGui). Tools/ compiles imgui sources
-- directly into its vcxproj and does not use this wrapper.
project "imgui"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp",
        "imgui_widgets.cpp", "imgui_demo.cpp",
        "imgui.h", "imgui_internal.h",
        "backends/imgui_impl_sdl3.cpp", "backends/imgui_impl_sdl3.h",
    }

    includedirs { ".", THIRDPARTY_SDL3_INCLUDE or "." }

    defines { "_CRT_SECURE_NO_WARNINGS" }

    filter "system:windows"
        systemversion "latest"
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
