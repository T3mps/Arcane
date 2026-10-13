set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PLATFORM_TOOLSET v143)

# SDL3 is the one vendor module several engine modules share (Window, Input,
# Render, ArcaneImGui): ONE instance per process means one SDL3.dll, staged
# beside every program whose module closure reaches it (build/modules.lua's
# prebuilt vendor entries). Every other port stays static.
if(PORT STREQUAL "sdl3")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
