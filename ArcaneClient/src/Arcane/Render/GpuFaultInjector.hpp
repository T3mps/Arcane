#pragma once

// GpuFaultInjector -- the deliberate GPU fault. DEV BUILDS ONLY.
//
// GPU crash diagnostics arc, Task 11: the desk battery
// (docs/specs/2026-08-11-gpu-crash-diagnostics-design.md, Testing) is written
// against a fault we can cause on demand, and this is what names it.
//
// ===================================================================
// THE INJECTOR IS NRI's NOW (NRI Phase 5a, Task 8b)
// ===================================================================
// This header used to declare an object: Create(nvrhi::IDevice*,
// ShaderLibrary&) built a compute pipeline, a sink UAV and a parameter CB, and
// Fire(nvrhi::ICommandList*) recorded the faulting dispatch into a caller's
// open list. That implementation (GpuFaultInjector.cpp) is DELETED, and
// nothing was ported, because a working replacement was already live:
// `Arcane::NriDiagnostics::FireFault(NriDevice&, nri::Queue&)`
// (Render/Nri/NriDiagnostics.cpp) dispatches the SAME
// data/shaders/gpu_fault.hlsl loop as a one-off NRI compute submit.
//
// THE ROUTING WAS ALREADY DONE, which is why no shim is left behind. Both
// hosts' `--crash-gpu` paths call FireFault directly and have since Tasks 4
// and 6 deleted their NVRHI arms (ArcaneRuntime/RuntimeFrame.cpp's
// `--crash-gpu N` block; ArcaneEditor's EditorApp::FireDeliberateGpuFault,
// reached from Build -> Diagnostics -> Crash GPU and from the scheduled
// `--crash-gpu N`). The nvrhi Create/Fire pair had ZERO callers by the time
// this task ran -- both hosts still declared a `unique_ptr<GpuFaultInjector>`
// member, and both had comments saying it was never built. Those members went
// with this class's body.
//
// TWO THINGS IN THE NRI ARM ARE LOAD-BEARING AND WERE FOUND THE HARD WAY
// (@21759d73). Neither is restated here, because both live at the code that
// depends on them and a second copy is a second thing to drift:
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
// WHAT SURVIVES HERE: the breadcrumb scope name, and only that. It is the
// payload the whole battery item reads back out of a `.arcdiag`, both arms
// spell it the same way, and a literal duplicated between the producer
// (NriDiagnostics::FireFault) and the readers (NriDiagnosticsTest) is exactly
// the drift this constant prevents.

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
