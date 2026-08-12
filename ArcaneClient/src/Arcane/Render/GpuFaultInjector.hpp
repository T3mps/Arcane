#pragma once

// GpuFaultInjector -- the deliberate GPU fault. DEV BUILDS ONLY.
//
// GPU crash diagnostics arc, Task 11: the desk battery
// (docs/specs/2026-08-11-gpu-crash-diagnostics-design.md, Testing) is written
// against a fault we can cause on demand, and this is the thing that causes it.
// Everything downstream of the fault already exists and is untouched by this
// file: NvrhiMessageCallback -> ObserveDeviceRemoved ->
// Diagnostics::WriteReport("gpu-crash: device removed") -> the GPU-section
// provider -> the `.arcdiag` + `.gpudump` pair -> the Problems notify ->
// CrashReportDocument.
//
// It dispatches ONE compute shader (data/shaders/gpu_fault.hlsl) whose two
// fault mechanisms and their very different guarantees are documented at length
// there. Short version: a CB-bounded serially dependent loop runs the GPU past
// the OS TDR window (the guaranteed, vendor-agnostic path), and the loop's
// un-elidable side effect is an out-of-bounds UAV store that MIGHT page-fault
// first on a path without a bounds check (the opportunistic path).
//
// THE ONLY COMPUTE PATH IN THE ENGINE. Before this arc the first-party tree had
// no compute pipeline, no UAV, and no dispatch of any kind -- every pass was
// raster. Nothing here is meant as a general compute facility: it is
// deliberately a closed, single-purpose object with no knobs, because a "reusable
// compute helper" whose only caller crashes the GPU would be a design fiction.
// When a real compute pass lands, it should grow its own seam and this file
// should not be its template.
//
// NVRHI IS NOT WRAPPED and no native handle is reached for -- the whole dispatch
// is portable nvrhi (createComputePipeline / setComputeState / dispatch), so the
// arc's `getNativeObject` gate stays satisfied and the SAME command works on
// both backends (desk battery item 4 runs it on Vulkan unchanged).

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>

namespace Arcane
{
    class ShaderLibrary;

#if !defined(ARCANE_DIST)

    class ARCANE_API GpuFaultInjector
    {
    public:
        // Builds the compute pipeline, the sink UAV, and the parameter CB.
        // Returns null (with ARC_ERROR on the failing step) if the compute
        // shader artifact is missing or any resource fails to create -- a
        // headless or degraded device makes this unavailable, not fatal.
        static std::unique_ptr<GpuFaultInjector> Create(nvrhi::IDevice* device,
                                                        ShaderLibrary& shaders);

        virtual ~GpuFaultInjector() = default;

        // Records the faulting dispatch into `commandList`, which MUST already
        // be open and must stay open until the caller closes it. The caller
        // submits: this only records, exactly like SelectionOutline::Render.
        //
        // The pass scope is opened INSIDE this call rather than by the caller,
        // which is a deliberate departure from every other pass in the engine.
        // Everywhere else the scope name is a label on work that would still be
        // correct without it; here the scope name IS the payload -- it is the
        // breadcrumb the crash report exists to show, and the entire point of
        // the battery item is reading "pass:gpu-fault" back out of a `.arcdiag`.
        // A caller that forgot the scope would produce a report that names
        // nothing and looks like the capture path is broken, so the one thing
        // that must not be forgettable is not left to the caller.
        virtual void Fire(nvrhi::ICommandList* commandList) = 0;

        // The breadcrumb scope Fire opens. Exposed so a test or a reader can
        // name it without duplicating the literal.
        static constexpr const char* kPassName = "pass:gpu-fault";
    };

#endif   // !ARCANE_DIST
}
