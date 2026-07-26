-- imgui-node-editor premake5 build script (Arcane workspace consumer).
-- MIT (thedmd/imgui-node-editor) -- node-graph canvas over Dear ImGui.
-- Used by the Arcane Editor's shader-graph document (shader-editor Slice 9).
-- The retired Tools/ editor compiled these sources directly into its vcxproj
-- and does not use this wrapper.
project "imgui-node-editor"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "imgui_node_editor.cpp", "imgui_node_editor.h",
        "imgui_node_editor_api.cpp", "imgui_node_editor_internal.h",
        "imgui_canvas.cpp", "imgui_canvas.h",
        "crude_json.cpp", "crude_json.h",
    }

    includedirs { ".", "../imgui" }

    defines { "_CRT_SECURE_NO_WARNINGS" }
    -- These objects link into a consumer EXE while imgui itself is exported
    -- from Arcane.dll -- the ImGui calls here must agree with the consumer's
    -- IMGUI_API (dllimport in the Arcane workspace; unset elsewhere). Distinct
    -- from THIRDPARTY_IMGUI_API, which the imgui static lib uses to EXPORT.
    if THIRDPARTY_NODE_EDITOR_IMGUI_API then
        defines { "IMGUI_API=" .. THIRDPARTY_NODE_EDITOR_IMGUI_API }
    end

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
