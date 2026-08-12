// Deliberate GPU fault -- the desk-battery trigger for the GPU crash
// diagnostics arc (docs/specs/2026-08-11-gpu-crash-diagnostics-design.md,
// Testing). NOT part of any render path: the only code that dispatches it is
// the dev-only fault command (Arcane/Render/GpuFaultInjector.hpp), which is
// compiled out of Dist entirely. The artifact still builds in Dist -- the
// batch script has no configuration awareness -- but nothing loads it there.
//
// TWO fault mechanisms in one dispatch, deliberately, because only ONE of them
// is actually guaranteed:
//
//   1. THE GUARANTEED ONE -- a long, serially dependent loop whose bound is a
//      constant-buffer value. dxc cannot fold the bound, cannot unroll the
//      loop, and cannot delete the body (a UAV store is a side effect), so the
//      dispatch really does run for as long as the CPU asks. Past the OS TDR
//      window (TdrDelay, 2 s by default on Windows) the watchdog resets the
//      adapter and every backend reports a removed/lost device. That path is
//      vendor- and driver-agnostic -- it is the OS, not the driver, that
//      decides -- which is why it is the one the command relies on.
//
//   2. THE OPPORTUNISTIC ONE -- the store inside the loop is out of bounds by
//      ~4 GiB. Do NOT read this as the primary mechanism: D3D12 and Vulkan
//      both REQUIRE a bounds check against the view for structured-buffer
//      access, so on a conformant device this write is discarded and faults
//      nothing at all. It is here because it costs nothing, because it is the
//      write that makes the loop un-elidable anyway, and because on any path
//      where the bounds check is absent it upgrades a timeout into a real page
//      fault with a DRED / VK_EXT_device_fault address to name.
//
// The OOB index is a CB value, not a literal, for the same reason the loop
// bound is: a compiler that could PROVE the access out of bounds would be
// entitled to fold the store away. It also keeps CPU-side validation quiet --
// an out-of-bounds STORE is a GPU-side condition that the D3D12 debug layer
// never sees, so nothing here is rejected before it reaches the GPU. (GPU-Based
// Validation would flag it, but Arcane never enables GBV: Device.hpp's
// enableD3D12DebugLayer is opt-in and off by default, and GBV is not wired at
// all.)
//
// The loop is BOUNDED rather than `while (true)`, on purpose: on a machine with
// TDR disabled (TdrLevel = 0) an unbounded loop is a hard hang needing a
// reboot, where this one drains on its own.

cbuffer FaultCB : register(b0)
{
    uint gIterations;   // loop bound -- CPU-supplied so dxc cannot fold it
    uint gOobElement;   // first OOB element index (far past gSink's length)
    uint gSeed;         // keeps the chain from being constant-folded
    uint gSinkMask;     // in-range mask for the one legitimate store
};

RWStructuredBuffer<uint> gSink : register(u0);

[numthreads(64, 1, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID)
{
    uint acc = gSeed + tid.x;

    // Serially dependent: iteration i+1 needs iteration i's result, so the
    // chain cannot be vectorised away or hidden behind other work, and the
    // dispatch's duration scales with gIterations no matter how wide the GPU
    // is. That is what makes the TDR window a reliable target rather than a
    // race against clock speed.
    [loop]
    for (uint i = 0u; i < gIterations; ++i)
    {
        acc = acc * 1664525u + 1013904223u;           // Numerical Recipes LCG
        gSink[gOobElement + (acc & 0xFFu)] = acc;     // (2) -- and the side effect keeping (1) alive
    }

    // One in-range store, so the result stays observable even for a future
    // driver that learns to elide provably discarded writes.
    gSink[tid.x & gSinkMask] = acc;
}
