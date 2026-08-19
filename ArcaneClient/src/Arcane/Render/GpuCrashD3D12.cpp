// GPU crash diagnostics arc (Task 5), reduced to its ONE surviving layer at
// NRI Phase 5a, Task 9.5a: the DRED policy tier.
//
// WHAT THIS FILE WAS. Three independent layers, each degrading to "off" with
// exactly one WARN rather than failing device creation: (1) MARKERS (F-1), a
// per-scope begin/end value the GPU wrote into process-owned system memory
// via ID3D12GraphicsCommandList2::WriteBufferImmediate; (2) DRED (F-2), below;
// (3) the `.gpudump` container's D3D12 sections -- raw marker bytes plus a
// flattening of DRED's pointer-linked breadcrumb/page-fault output.
//
// WHAT SURVIVES, AND WHY ONLY THIS. Layers 1 and 3 lived inside
// D3D12CrashBackend, which took an nvrhi::IDevice* and reached the native
// ID3D12Device/ID3D12GraphicsCommandList through getNativeObject. Its only
// constructor, MakeD3D12CrashBackend, was called by the NVRHI device layer
// alone -- deleted at Task 8b -- so the whole backend had been unreachable
// since. It is deleted here rather than left as unreachable NVRHI-typed code;
// git history is the reference copy, and IGpuCrashBackend.hpp carries the
// named capability loss (no DRED breadcrumb or page-fault READBACK exists on
// the NRI path; Phase 4's native marker layer inherits it).
//
// EnableD3D12Dred() below is NOT part of that loss and must not be confused
// with it. It is armed unconditionally by DeviceCreationD3D12.cpp before
// D3D12CreateDevice, is process-global rather than device-scoped, touches no
// NVRHI type, and its tier choice is pinned by DiagnosticsTest's `[diag]`
// case in all three configs. Task 1's Dist correction -- full auto-breadcrumbs
// instead of markers-only, because markers-only with no marker producer yields
// an EMPTY breadcrumb list -- lives in its #if below, byte-unchanged here.

#include <Arcane/Render/IGpuCrashBackend.hpp>

#include <Arcane/Base/Log.hpp>

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace Arcane
{
    namespace
    {
        // ------------------------------------------------------------------
        // DRED policy tier (F-2) -- decided once, before any device exists
        // ------------------------------------------------------------------

        std::once_flag           g_dredOnce;
        std::atomic<const char*> g_dredTier{ "dred:off" };
    }

    // -------------------------------------------------------------------------
    // Public entry points
    // -------------------------------------------------------------------------

    const char* DredTier()
    {
        return g_dredTier.load(std::memory_order_acquire);
    }

    void EnableD3D12Dred()
    {
        std::call_once(g_dredOnce, []() {
            // F-2b: "DRED settings are global to the process, and you must
            // configure them prior to creating a Direct3D 12 Device."
            // D3D12GetDebugInterface is the retrieval path for the SETTINGS
            // object; it does not enable the debug layer, so this is
            // independent of RenderDeviceDesc::enableD3D12DebugLayer.
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
            if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
            {
                ARC_WARN("DRED unavailable: D3D12GetDebugInterface for "
                         "ID3D12DeviceRemovedExtendedDataSettings failed; continuing without "
                         "GPU breadcrumbs");
                g_dredTier.store("dred:off", std::memory_order_release);
                return;
            }

            settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);

            // F-2d: the Settings2 -> Settings1 -> Settings ladder. Only the
            // Settings1 rung may call SetBreadcrumbContextEnablement, only the
            // Settings2 rung may call UseMarkersOnlyAutoBreadcrumbs; the
            // headers on this machine carry both, the RUNTIME may not.
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings1;
            const bool hasSettings1 = SUCCEEDED(settings.As(&settings1));
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings2> settings2;
            const bool hasSettings2 = SUCCEEDED(settings.As(&settings2));

            // PIX marker/event strings inside the breadcrumb list (DRED 1.2).
            // Wanted in every tier: it is what turns a breadcrumb op stream
            // into named scopes, and it is the ONLY readable content the
            // markers-only tier produces at all.
            //
            // The WARN sits ABOVE the tier split on purpose: losing Settings1
            // degrades BOTH arms, and it hurts the Dist arm hardest (numeric-
            // only ops from an already-sparse breadcrumb list). Every tier
            // label below therefore carries a `-nocontext` variant, so
            // activeLayers distinguishes the degrade in either build.
            if (hasSettings1)
            {
                settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            }
            else
            {
                ARC_WARN("DRED Settings1 unavailable (no SetBreadcrumbContextEnablement); "
                         "breadcrumbs will carry no PIX marker strings (F-2d)");
            }

#if defined(ARCANE_DIST)
            // F-2c (lightweight tier) is SUSPENDED as of Phase 5a. Its
            // markers-only auto-breadcrumbs are only worth anything if pass
            // scopes also emit nvrhi::ICommandList::beginMarker/endMarker
            // (F-2c-bis, GpuInstrumentation.hpp) -- and after the NVRHI
            // deletion there is no nvrhi command list to emit them on, while
            // the NRI path's WriteMarkerNative is still the stub at
            // NriDiagnostics.cpp:82. Selecting markers-only here would produce
            // an EMPTY breadcrumb list, which the header calls strictly worse
            // than no DRED at all -- and it would do it silently, in the one
            // config nobody runs interactively.
            //
            // So Dist takes full auto-breadcrumbs: heavier than F-2c intended,
            // but it actually records something. RESTORE the lighter tier when
            // the native NRI marker layer lands and GpuPassScope emits through
            // it -- that arc is what re-earns this branch.
            settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_OFF);
            g_dredTier.store(hasSettings1 ? "dred:breadcrumbs" : "dred:breadcrumbs-nocontext",
                             std::memory_order_release);
            (void)hasSettings2;
#else
            settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            g_dredTier.store(hasSettings1 ? "dred:full" : "dred:full-nocontext",
                             std::memory_order_release);
            (void)hasSettings2;
#endif
            ARC_INFO("DRED enabled: {}", g_dredTier.load(std::memory_order_acquire));
        });
    }

}
