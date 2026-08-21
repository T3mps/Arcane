#pragma once

// GpuFaultInjector -- the deliberate GPU fault. DEV BUILDS ONLY.
//
// The desk battery
// (docs/specs/2026-08-11-gpu-crash-diagnostics-design.md, Testing) is written
// against a fault we can cause on demand, and this is what names it.
//
// ===================================================================
// THE INJECTOR ITSELF IS NriDiagnostics::FireFault
// ===================================================================
// `Arcane::NriDiagnostics::FireFault(NriDevice&, nri::Queue&)`
// (Render/Nri/NriDiagnostics.cpp) dispatches the data/shaders/gpu_fault.hlsl
// loop as a one-off NRI compute submit. Both hosts' `--crash-gpu` paths call
// it directly (ArcaneRuntime/RuntimeFrame.cpp's `--crash-gpu N` block;
// ArcaneEditor's EditorApp::FireDeliberateGpuFault, reached from Build ->
// Diagnostics -> Crash GPU and from the scheduled `--crash-gpu N`).
//
// TWO THINGS IN THAT INJECTOR ARE LOAD-BEARING AND WERE FOUND THE HARD WAY.
// Neither is restated here, because both live at the code that depends on
// them and a second copy is a second thing to drift:
//   * the 256-byte constant-buffer/CBV size (kFaultCBSize, NriDiagnostics.cpp)
//     -- D3D12 requires a CBV's SizeInBytes to be a multiple of 256, and a
//     16-byte view aborted the process before the injector could dispatch;
//   * the non-null AbortExecution callback in the NRI callback interface
//     (NriAbortExecution, wired by MakeNriCallbacks in NriCommon.cpp) -- NRI
//     invokes the slot after every non-SUCCESS ReportMessage, and leaving it
//     NULL does not opt out: NRI fills a null slot with its own DebugBreak
//     default, which turned a recoverable dx12 failure inside the injector
//     into an unhandled STATUS_BREAKPOINT that killed the process before the
//     host could report anything. An explicit no-op is the only real opt-out.
//
// WHAT THE FAULT IS: one compute dispatch of data/shaders/gpu_fault.hlsl,
// whose two mechanisms and their very different guarantees are documented at
// length there. Short version: a CB-bounded serially dependent loop runs the
// GPU past the OS TDR window (the guaranteed, vendor-agnostic path), and the
// loop's un-elidable side effect is an out-of-bounds UAV store that MIGHT
// page-fault first on a path without a bounds check (the opportunistic path).
//
// WHAT LIVES HERE: the breadcrumb scope name, and only that. It is the
// payload the whole battery item reads back out of a `.arcdiag`, and a
// literal duplicated between the producer (NriDiagnostics::FireFault) and the
// readers (NriDiagnosticsTest) is exactly the drift this constant prevents.

#include <Arcane/Base/Api.hpp>

namespace Arcane
{
#if !defined(ARCANE_DIST)

    namespace GpuFaultInjector
    {
        // The breadcrumb scope FireFault opens around the faulting dispatch.
        //
        // The scope is opened INSIDE the dispatch function rather than by its
        // caller, which is a deliberate departure from every other pass in the
        // engine. Everywhere else the scope name is a label on work that would
        // still be correct without it; here the scope name IS the payload --
        // it is the breadcrumb the crash report exists to show, and the entire
        // point of the battery item is reading "pass:gpu-fault" back out of a
        // `.arcdiag`. A caller that forgot the scope would produce a report
        // that names nothing and looks like the capture path is broken, so the
        // one thing that must not be forgettable is not left to the caller.
        inline constexpr const char* kPassName = "pass:gpu-fault";
    }

#endif   // !ARCANE_DIST
}
