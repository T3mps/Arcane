#pragma once

// THE RENDER-LAYER ERROR LATCH -- the 0/0 gate's one counter, and the one
// device-removed hook slot.
//
// NRI Phase 5a, Task 8a: this was `NvrhiMessageCallback` (storage) plus five
// exported accessors in `Render/Device.{hpp,cpp}`. Both of those files
// belonged to the NVRHI device layer the phase deleted (Task 8b), and this
// latch does not: it is the instrument every host, every GPU test and every
// desk battery in the phase reads (`RenderErrorCount B -> N`), and every graph-side producer --
// ARC_NRI_CHECK, the executor, the nodes, the pipeline cache, the ImGui
// layer -- bumps it. So it moved to a home with NO nvrhi dependency, ahead of
// the deletion rather than during it.
//
// Nothing about the storage changed in that move. It is still a header-only
// function-local-static singleton, driven only from inside ArcaneClient.dll,
// with the same two atomics and the same public seams -- which is why the
// exported *ForTest shims below still exist and still say what they always
// said.
//
// `NvrhiMessageCallback` survived Task 8a as a thin, stateless
// nvrhi::IMessageCallback adapter over this class; Task 8b deleted it with the
// rest of the NVRHI device layer, because the only thing that ever installed
// it was the nvrhi device desc. Nothing else about this file changed.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Base/Log.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace Arcane
{
    class RenderErrorLatch final
    {
    public:
        // F-3b: a device loss is reported through THIS sink, on submit, on
        // both backends -- which makes it the earliest cross-backend
        // observation point (it fires before the next Present), so a GPU crash
        // backend installs a hook here rather than inventing a second,
        // backend-shaped detection path.
        //
        // NVRHI was the original producer: it caught the loss on submit and
        // reported the literal text "Device Removed!"
        // (ThirdParty/nvrhi/src/d3d12/d3d12-device.cpp:630 and
        // .../vulkan/vulkan-queue.cpp:200), which is the string
        // NotifyIfDeviceRemoved below still scans for. Since Task 8b deleted
        // that layer the producers are NRI's own: ARC_NRI_CHECK's typed
        // DEVICE_LOST branch (NoteDeviceLost, which does not depend on message
        // text at all) and the substring scan, still fed by RouteNriError.
        //
        // Raw function pointer + last-writer-wins, mirroring the diagnostics
        // Sink slot. The hook must be cleared by whoever installed it, before
        // the object it captures dies.
        using DeviceRemovedHook = void (*)();

        static RenderErrorLatch& Instance()
        {
            static RenderErrorLatch s_instance;
            return s_instance;
        }

        uint64_t ErrorCount() const { return m_errorCount.load(); }

        // Test support ONLY: the counter is documented (below, on
        // RenderErrorCount) as "since process start" and production code must
        // never call this -- it exists so a test that deliberately drives an
        // Error/Fatal through this sink (proving a discipline macro or
        // callback wiring actually reaches the shared 0/0 gate latch, e.g.
        // NriCommon's ARC_NRI_CHECK) can restore the latch afterward instead
        // of permanently failing every OTHER test case's RenderErrorCount()==0
        // assertion for the rest of the process. Same idiom as
        // GpuInstrumentation's ResetGpuDeviceLost(), reused by
        // GpuCrashReportTest.cpp for the same "other cases must not leak
        // into this one" reason.
        void ResetForTest() noexcept { m_errorCount.store(0); }

        void SetDeviceRemovedHook(DeviceRemovedHook hook) noexcept
        {
            m_deviceRemovedHook.store(hook, std::memory_order_release);
        }

        // What the slot currently holds, or null. Read-only and side-effect
        // free -- it never invokes the hook. Exists because the slot is
        // last-writer-wins. It had TWO writers between Phase 3 and Phase 5a
        // (the NVRHI device layer and Render/Nri/NriDiagnostics); Task 8b left
        // NriDiagnostics as the only one. "Did arming actually install one,
        // and did a second arm leave it alone" stays a property worth
        // pinning, and it is unobservable without a reader.
        // (Named Current* because a member called `DeviceRemovedHook` would
        // shadow the type alias above inside this class.)
        [[nodiscard]] DeviceRemovedHook CurrentDeviceRemovedHook() const noexcept
        {
            return m_deviceRemovedHook.load(std::memory_order_acquire);
        }

        // Phase 2, Task 1: the TAGGED error seam, for every producer that is
        // not the substring path below -- the D3D12 debug layer's InfoQueue1
        // callback, and the frame graph's `NoteError("nri-graph", ...)`. It
        // gives such a producer the two things it needs and withholds the one
        // it must not have:
        //
        //   - It bumps the SAME atomic RenderErrorCount() reads, so a D3D12
        //     VUID or a graph refusal fails the 0/0 gate exactly like any
        //     other render-layer error. That is the whole point of the latch.
        //   - It logs under the producer's OWN tag. The D3D12 callback used
        //     to reach the latch by calling the substring seam below, which
        //     printed debug-layer text as "[nvrhi] ..." -- a lie about the
        //     message's origin, and the first thing to mislead whoever reads
        //     the log. (That seam is NoteNriError now, and tags "[nri]"; see
        //     its own comment for the tag-honesty caveat that remains.)
        //   - It deliberately does NOT run NotifyIfDeviceRemoved. That hook
        //     writes a diagnostic report and a minidump; the D3D12 debug
        //     layer invokes its message callback on whatever thread tripped
        //     the error, from INSIDE a D3D12 call, so letting a "Device
        //     Removed" substring there re-enter removal handling is a
        //     re-entrancy hazard for no gain. The submit-time feed (see
        //     NoteNriError and NoteDeviceLost) stays the ONE device-removed
        //     observation point -- F-3b, and the reason the hook lives there.
        void NoteError(const char* tag, const char* text) noexcept
        {
            ++m_errorCount;
            ARC_ERROR("[{}] {}", tag ? tag : "?", text ? text : "");
        }

        // Phase 2, Task 6: the TYPED device-lost seam. NoteError above
        // deliberately withholds the removal hook because its producer (the
        // D3D12 debug layer) reports from inside a D3D12 call on whatever
        // thread tripped the error. This one is for the opposite case: a
        // producer that holds a TYPED result -- nri::Result::DEVICE_LOST off
        // its own QueueSubmit/AcquireNextTexture/QueuePresent call, on its
        // own render thread, outside any driver callback -- where firing the
        // hook is both safe and the whole point.
        //
        // It exists because the substring path cannot serve that producer:
        // NotifyIfDeviceRemoved matches NVRHI's literal "Device Removed!"
        // text, and NRI's failure text says "DEVICE_LOST", so before this an
        // NRI-path device loss reached the latch but NEVER
        // ObserveDeviceRemoved -- i.e. no .arcdiag, no .gpudump, no host
        // shutdown latch. Same counter, same hook, one honest tag.
        void NoteDeviceLost(const char* tag, const char* text) noexcept
        {
            ++m_errorCount;
            ARC_ERROR("[{}] {}", tag ? tag : "?", text ? text : "");
            if (const DeviceRemovedHook hook = m_deviceRemovedHook.load(std::memory_order_acquire))
                hook();
        }

        // THE SUBSTRING SEAM -- what nvrhi::IMessageCallback::message()'s
        // Error/Fatal branches WERE, lifted out of NvrhiMessageCallback (Task
        // 8a) so that a caller with no nvrhi types can still reach it.
        //
        // TWO producers route here now. NriCommon's RouteNriError -- what
        // every ARC_NRI_CHECK failure funnels into, and which has called this
        // exact code path since the substrate landed (@81754283) precisely so
        // the NRI arm and the NVRHI arm shared one counter and one removal
        // scan; and DeviceCreationVulkan's VkDebugCallback, which reached it
        // through the nvrhi adapter until Task 8b deleted that adapter and
        // repointed the one call. NVRHI's own message callback was the third
        // and went with the device layer.
        //
        // RETAGGED "[nvrhi]" -> "[nri]", AND RENAMED, at NRI Phase 5a Task 11a.
        // Tasks 8a and 8b deliberately left both alone (a relocation must move
        // zero observables; deleting a layer is not a reason to rewrite log
        // text), deferring the vocabulary change to Task 11. NVRHI is gone from
        // the tree entirely by then, so the old name and tag pointed at a layer
        // that does not exist. Nothing automated consumed the tag: the D5a-1
        // battery matches only `RenderErrorCount N -> N`, no ArcaneTests case
        // asserts on the tag text, and no in-repo script greps it.
        //
        // TAG-HONESTY CAVEAT, stated rather than hidden: of the two producers
        // that route here, RouteNriError IS NRI, but DeviceCreationVulkan's
        // VkDebugCallback is the VULKAN VALIDATION LAYER reporting on a device
        // NRI created. "[nri]" names the render path rather than the exact
        // producer -- strictly better than "[nvrhi]", which named neither. It
        // also splits that one callback's output: its warning branch already
        // logs ARC_WARN("[vk] ...") (DeviceCreationVulkan.cpp:183) while its
        // errors leave here tagged "[nri]".
        //
        // DO NOT "FIX" THAT BY REPOINTING THE VULKAN PRODUCER AT NoteError.
        // NoteError(tag, text) does take a per-producer tag, but it
        // DELIBERATELY does not run NotifyIfDeviceRemoved (see its own comment
        // above), and this substring scan is one of the TWO feeds driving that
        // backend's device-removed hook -- DeviceCreationVulkan.cpp:119-127
        // names both. Swapping the call would buy a "[vk]" tag and silently
        // disarm the .arcdiag/.gpudump/shutdown-latch path for Vulkan device
        // loss. Giving THIS seam its own tag parameter is the shape that works.
        void NoteNriError(const char* messageText) noexcept
        {
            ++m_errorCount;
            ARC_ERROR("[nri] {}", messageText);
            NotifyIfDeviceRemoved(messageText);
        }

        // NO CALLERS as of Task 11a (NVRHI's message callback was the only one,
        // and it went with the device layer at Task 8b). Kept as the Fatal twin
        // of the seam above rather than deleted, since deleting it is not a
        // retag; flagged in that task's report as a deletion candidate.
        void NoteNriFatal(const char* messageText) noexcept
        {
            ++m_errorCount;
            ARC_CRITICAL("[nri] {}", messageText);
            NotifyIfDeviceRemoved(messageText);
        }

    private:
        void NotifyIfDeviceRemoved(const char* messageText)
        {
            // Substring, not equality: NVRHI (fully removed from the tree
            // now) used to report exactly "Device Removed!"; today's NRI
            // callback interface, the D3D12 debug layer and the Vulkan
            // validation messenger all wrap that same phrase with their own
            // prefixes and context, so an exact-equality match would miss
            // every producer left standing.
            if (!messageText || std::strstr(messageText, "Device Removed") == nullptr)
                return;
            if (const DeviceRemovedHook hook = m_deviceRemovedHook.load(std::memory_order_acquire))
                hook();
        }

        std::atomic<uint64_t>          m_errorCount{0};
        std::atomic<DeviceRemovedHook> m_deviceRemovedHook{nullptr};
    };

    // Total render-layer Error/Fatal diagnostics since process start, across
    // all devices and EVERY producer -- not just NVRHI's own message callback:
    // the Vulkan debug messenger, the D3D12 debug layer's InfoQueue1 callback,
    // NRI's callback interface and ARC_NRI_CHECK, and anything else reporting
    // through RenderErrorLatch::NoteError all land in this one counter.
    // GPU tests assert this stays zero -- the machine-enforced form of the
    // "validation must stay silent" foundation rule.
    ARCANE_API uint64_t RenderErrorCount();

    // Test support ONLY -- production code must never call this (the count
    // above is documented as "since process start"). Restores the 0/0 gate
    // latch to zero; exists so a test that deliberately trips it (proving a
    // discipline macro reaches the real, shared latch rather than a fake
    // local counter) can clean up after itself instead of leaking a
    // permanent +1 into every unrelated test case's RenderErrorCount()==0
    // assertion for the rest of the process. Same idiom as
    // ResetGpuDeviceLost() (GpuInstrumentation.hpp).
    ARCANE_API void ResetRenderErrorCount();

    // Test support ONLY -- the two seams a [nri] case needs to prove that
    // RenderErrorLatch::NoteError reaches THIS latch (and that it does
    // NOT fire the device-removed hook). They exist because
    // RenderErrorLatch is a header-only singleton -- a function-local
    // static in RenderErrorLatch.hpp -- so a test exe that included that
    // header would drive its OWN instance while RenderErrorCount(), exported
    // from ArcaneClient.dll, kept reading the DLL's. Production code inside
    // the DLL calls RenderErrorLatch::Instance().NoteError directly and
    // must never reach for these.
    ARCANE_API void NoteRenderErrorForTest(const char* tag, const char* text) noexcept;

    // Installs (or, with nullptr, clears) the device-removed hook on the
    // DLL-side RenderErrorLatch. Last-writer-wins, exactly like the
    // class's own setter -- so a test MUST clear it before the function it
    // names goes out of scope, and must not run while a real device holds
    // the slot (no device exists in the ~[gpu] gate, which is where the one
    // caller lives).
    ARCANE_API void SetRenderDeviceRemovedHookForTest(void (*hook)()) noexcept;

    // Reads the same DLL-side slot back, without ever invoking the hook.
    // Test support ONLY, and for the same header-only-singleton reason as the
    // setter above. The slot is last-writer-wins, and had TWO writers between
    // Phase 3's one-device flip and Phase 5a Task 8b -- the NVRHI device
    // layer's Init/dtor, and Render/Nri/NriDiagnostics::Arm/Disarm. Only the
    // latter is left, but "arming installed one" and "a second arm left it
    // alone" are still properties a headless case can only state if it can
    // read the slot (NriDiagnosticsTest pins both). Production code has no business reading
    // it: the hook exists to be CALLED by RenderErrorLatch, by nobody
    // else.
    [[nodiscard]] ARCANE_API void (*RenderDeviceRemovedHookForTest() noexcept)();
}
