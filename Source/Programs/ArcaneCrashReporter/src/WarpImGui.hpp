#pragma once

struct ImDrawData;

namespace Arcane::Reporter
{
    // Dear ImGui on a Direct3D 11 WARP device: Microsoft's software
    // rasterizer, not the GPU. A crash reporter cannot depend on the device
    // that just died, and a hand-rolled CPU blit of every hover was both
    // laggy and flashed the window's light class brush on focus.
    struct WarpImGui;

    [[nodiscard]] WarpImGui* WarpCreate(void* hwnd);
    void                     WarpDestroy(WarpImGui* warp);   // imgui backend shutdown, then the device
    void                     WarpNewFrame(WarpImGui* warp);
    [[nodiscard]] bool       WarpResize(WarpImGui* warp, int width, int height);
    void                     WarpDraw(WarpImGui* warp, ImDrawData* data);
}
