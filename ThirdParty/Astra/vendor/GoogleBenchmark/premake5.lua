project "GoogleBenchmark"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "on"
    location "../../ide"  -- Project files go to ide folder
    
    targetdir ("../../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../../bin-int/" .. outputdir .. "/%{prj.name}")
    
    files
    {
        "src/**.cc",  -- Include all .cc files
        "src/**.h"    -- Include all headers for IDE visibility
    }
    
    -- Exclude the main function if we want to provide our own
    removefiles { "src/benchmark_main.cc" }
    
    includedirs
    {
        "include",
        "src"
    }
    
    defines
    {
        "BENCHMARK_STATIC_DEFINE"  -- Building as static lib
    }
    
    filter "system:windows"
        systemversion "latest"
        defines { 
            "HAVE_STD_REGEX",
            "_CRT_SECURE_NO_WARNINGS",  -- Disable CRT security warnings
            "_SILENCE_TR1_NAMESPACE_DEPRECATION_WARNING"
        }
    
    filter "system:linux or system:macosx"
        defines { 
            "HAVE_STD_REGEX",
            "HAVE_PTHREAD"
        }
        links { "pthread" }
        
    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"
        optimize "off"
        defines { "DEBUG", "_DEBUG" }
        
    filter "configurations:Release"
        runtime "Release"
        optimize "speed"
        symbols "on"
        defines { "NDEBUG" }
        
    filter "configurations:Dist"
        runtime "Release"
        optimize "full"
        symbols "off"
        defines { "NDEBUG" }