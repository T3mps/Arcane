#include "WarpImGui.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <d3d11.h>
#include <dxgi.h>

namespace Arcane::Reporter
{
    struct WarpImGui
    {
        ID3D11Device*           device = nullptr;
        ID3D11DeviceContext*    context = nullptr;
        IDXGISwapChain*         swap = nullptr;
        ID3D11RenderTargetView* target = nullptr;
        bool                    backend = false;
        int                     width = 0;
        int                     height = 0;
    };

    namespace
    {
        void ReleaseTarget(WarpImGui* warp)
        {
            if (!warp->target) return;
            if (warp->context) warp->context->OMSetRenderTargets(0, nullptr, nullptr);
            warp->target->Release();
            warp->target = nullptr;
        }

        bool MakeTarget(WarpImGui* warp)
        {
            ReleaseTarget(warp);
            ID3D11Texture2D* back = nullptr;
            if (FAILED(warp->swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
            const HRESULT hr = warp->device->CreateRenderTargetView(back, nullptr, &warp->target);
            D3D11_TEXTURE2D_DESC desc{};
            back->GetDesc(&desc);
            back->Release();
            if (FAILED(hr) || !warp->target) return false;
            warp->width = static_cast<int>(desc.Width);
            warp->height = static_cast<int>(desc.Height);
            return true;
        }

        bool CreateDevice(HWND hwnd, WarpImGui* warp, DXGI_SWAP_EFFECT effect, int width, int height)
        {
            DXGI_SWAP_CHAIN_DESC sd{};
            sd.BufferCount = (effect == DXGI_SWAP_EFFECT_DISCARD) ? 1u : 2u;
            sd.BufferDesc.Width = static_cast<UINT>(width);
            sd.BufferDesc.Height = static_cast<UINT>(height);
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferDesc.RefreshRate.Numerator = 60;
            sd.BufferDesc.RefreshRate.Denominator = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = hwnd;
            sd.SampleDesc.Count = 1;
            sd.Windowed = TRUE;
            sd.SwapEffect = effect;

            const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
            D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
            const HRESULT hr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                levels, 2, D3D11_SDK_VERSION,
                &sd, &warp->swap, &warp->device, &got, &warp->context);
            return SUCCEEDED(hr) && warp->swap && warp->device && warp->context;
        }
    }

    WarpImGui* WarpCreate(void* hwnd)
    {
        HWND window = static_cast<HWND>(hwnd);
        if (!window) return nullptr;
        RECT rc{};
        GetClientRect(window, &rc);
        const int width = rc.right > 0 ? rc.right : 1;
        const int height = rc.bottom > 0 ? rc.bottom : 1;

        WarpImGui* warp = new WarpImGui();
        // Flip discard is the DWM path (no blit, no class-brush wipe). The
        // older discard effect is the fallback if this Windows build refuses
        // the flip model on a legacy swap-chain description.
        if (!CreateDevice(window, warp, DXGI_SWAP_EFFECT_FLIP_DISCARD, width, height))
        {
            if (warp->swap) warp->swap->Release();
            if (warp->context) warp->context->Release();
            if (warp->device) warp->device->Release();
            warp->swap = nullptr;
            warp->context = nullptr;
            warp->device = nullptr;
            if (!CreateDevice(window, warp, DXGI_SWAP_EFFECT_DISCARD, width, height))
            {
                if (warp->swap) warp->swap->Release();
                if (warp->context) warp->context->Release();
                if (warp->device) warp->device->Release();
                delete warp;
                return nullptr;
            }
        }
        if (!MakeTarget(warp) || !ImGui_ImplDX11_Init(warp->device, warp->context))
        {
            WarpDestroy(warp);
            return nullptr;
        }
        warp->backend = true;

        IDXGIDevice* dxgi = nullptr;
        if (SUCCEEDED(warp->device->QueryInterface(IID_PPV_ARGS(&dxgi))) && dxgi)
        {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && adapter)
            {
                IDXGIFactory* factory = nullptr;
                if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))) && factory)
                {
                    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
                    factory->Release();
                }
                adapter->Release();
            }
            dxgi->Release();
        }
        return warp;
    }

    void WarpDestroy(WarpImGui* warp)
    {
        if (!warp) return;
        if (warp->backend)
        {
            ImGui_ImplDX11_Shutdown();
            warp->backend = false;
        }
        ReleaseTarget(warp);
        if (warp->swap) warp->swap->Release();
        if (warp->context) warp->context->Release();
        if (warp->device) warp->device->Release();
        delete warp;
    }

    void WarpNewFrame(WarpImGui* warp)
    {
        if (warp && warp->backend) ImGui_ImplDX11_NewFrame();
    }

    bool WarpResize(WarpImGui* warp, int width, int height)
    {
        if (!warp || !warp->swap || width <= 0 || height <= 0) return warp && warp->target;
        if (width == warp->width && height == warp->height && warp->target) return true;
        ReleaseTarget(warp);
        if (FAILED(warp->swap->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                             DXGI_FORMAT_UNKNOWN, 0)))
            return false;
        return MakeTarget(warp);
    }

    void WarpDraw(WarpImGui* warp, ImDrawData* data)
    {
        if (!warp || !warp->context || !warp->target || !data) return;
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(warp->width);
        vp.Height = static_cast<float>(warp->height);
        vp.MaxDepth = 1.0f;
        warp->context->RSSetViewports(1, &vp);
        warp->context->OMSetRenderTargets(1, &warp->target, nullptr);
        // Editor panel tone, so a pixel ImGui does not cover is not white.
        const float panel[4] = { 30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f };
        warp->context->ClearRenderTargetView(warp->target, panel);
        ImGui_ImplDX11_RenderDrawData(data);
        warp->swap->Present(0, 0);   // no vsync: a dialog should follow the mouse, not the refresh
    }
}
