// The D3D12 CREATION HALF: DXGI factory + adapter + device + direct queue,
// the process-global debug-layer sequencing around them, DRED enablement, and
// the InfoQueue arming that gives the D3D12 debug layer a channel into our log
// and the error latch. No NVRHI: everything here is native D3D12.
//
// NRI Phase 5a, Task 8a moved this out of DeviceD3D12.cpp verbatim. That file
// is the NVRHI device wrapper the phase deletes, and this code is not about
// NVRHI at all -- both consumers of CreateD3D12NativeDevice (the NVRHI device,
// and Nri/NriDevice.cpp's wrapper path) call the SAME function, so the D3D12
// debug/DRED story was already shared since Phase 1 Task 7; only its address
// was wrong. Nothing below changed except its file and the two lines that now
// name RenderErrorLatch instead of NvrhiMessageCallback: same calls, same
// order, same log lines, same early returns.
//
// NRI Phase 5a, Task 8b then deleted DeviceD3D12.cpp and moved the F-3
// device-removed observer down here too -- it had to travel WITH the deletion
// of the installer that used to sit beside it, because moving it earlier
// would have changed the function-pointer value in the hook slot that
// NriDiagnostics::Disarm compares against.
//
// See DeviceCreationD3D12.hpp for the two-consumer shape and the member-order
// rule. The Vulkan twin (CreateVulkanNativeDevice, and the VkDebugCallback
// that is this file's messenger analogue) got the same treatment in the same
// task and now lives in DeviceCreationVulkan.cpp -- the tree is symmetric.

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/DeviceCreationD3D12.hpp>
#include <Arcane/Render/DeviceRemovedObservers.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // NoteGpuDeviceLost -- the host's device-lost latch
#include <Arcane/Render/IGpuCrashBackend.hpp>   // EnableD3D12Dred -- the F-2 DRED tier, armed before D3D12CreateDevice
#include <Arcane/Render/RenderErrorLatch.hpp>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdlib>
#include <iterator>
#include <string>

using Microsoft::WRL::ComPtr;

namespace Arcane
{
    namespace
    {
        // ------------------------------------------------------------------
        // Process-global D3D12 debug-layer state (NRI Phase 2, D1 shakedown).
        // ------------------------------------------------------------------
        // ID3D12Debug::EnableDebugLayer is a BEFORE-ANY-DEVICE call, and the
        // documentation is explicit about what happens otherwise: "To enable
        // the debug layers using this API, it must be called before the D3D12
        // device is created. Calling this API after creating the D3D12 device
        // will cause the D3D12 runtime to remove the device."
        // (learn.microsoft.com, ID3D12Debug::EnableDebugLayer, Remarks.)
        //
        // That is exactly what the `--nri-graph` vehicle did on its first desk
        // run, back when the engine still booted an NVRHI device of its own:
        // that device came up with the layer OFF (the flag defaults false --
        // RenderDeviceDesc.hpp, the Nahimic-OSD fail-fast hazard), then the
        // vehicle ran THIS SAME creation half a second time with
        // enableD3D12DebugLayer forced true, so EnableDebugLayer landed on a
        // process that already owned a live device. Every dx12 vehicle run then
        // failed at D3D12CreateDevice and exited 1 after 0 frames.
        //
        // These two flags make the sequencing structural rather than a rule
        // somebody has to remember at each new call site.
        //
        // MONOTONE ON PURPOSE, and the conservative direction:
        // `g_d3d12DeviceCreated` is never cleared. It was never cleared because
        // the NVRHI owner released its device through MEMBER DESTRUCTION rather
        // than DestroyD3D12NativeDevice; that owner is gone as of Phase 5a Task
        // 8b and every remaining owner (NativeDeviceOwner) does route teardown
        // through DestroyD3D12NativeDevice, so a decrement is now expressible
        // -- but it is deliberately still not done, because the failure mode of
        // being wrong here is asymmetric. Skipping an enable that would have
        // been legal costs one diagnostic channel; making the call when it is
        // NOT legal removes a live device. Nothing in the tree creates a second
        // D3D12 device at all since the one-device flip, so today this costs
        // nothing.
        std::atomic<bool> g_d3d12DeviceCreated{ false };
        std::atomic<bool> g_d3d12DebugLayerEnabled{ false };

        // F-3: the ONE device-removed observation point for this backend.
        // It is reached through RenderErrorLatch's hook slot, which
        // Render/Nri/NriDiagnostics::Arm fills with ObserveDeviceRemovedD3D12
        // below. Two producers drive that slot: the latch's "Device Removed"
        // substring scan (NoteNriError -- what NriCommon's RouteNriError
        // funnels every ARC_NRI_CHECK failure into), and its TYPED seam
        // NoteDeviceLost, which is what NriDiagnostics' `--crash-gpu`
        // removal poll calls once GetDeviceRemovedReason answers.
        //
        // NRI Phase 5a, Task 8b: this observer moved here FROM DeviceD3D12.cpp
        // with that file's deletion, in one step -- moving it earlier would
        // have changed which function pointer the hook slot holds, which
        // NriDiagnostics::Disarm compares against. The two observables the
        // old paragraph here named -- F-3b's NVRHI submit-time message hook,
        // and F-3c's DXGI Present -- are both gone with the NVRHI layer.
        //
        // Once-only per armed device: a removed device keeps reporting removal
        // on every submit, and the second report is worthless -- the marker
        // buffer and DRED state belong to the FIRST one. Reset when a new
        // backend arms (project switch recreates the device).
        std::atomic<bool> g_deviceRemovedReported{ false };

        void ObserveDeviceRemoved()
        {
            if (g_deviceRemovedReported.exchange(true, std::memory_order_acq_rel))
                return;

            // The reason string is load-bearing: Diagnostics::DeriveKind
            // classifies the .arcdiag "kind" by case-sensitive substring, and
            // only a reason containing lowercase "gpu" resolves to a gpu kind
            // (here "gpu-crash", which is what makes the .gpudump sibling get
            // written). Do not reword.
            Diagnostics::WriteReport("gpu-crash: device removed");

            // AFTER the report, deliberately: hosts poll this latch and shut
            // down on it, and "observed" must always mean "the report exists".
            NoteGpuDeviceLost();
        }

        // Phase 2, Task 1 instrumentation. WHICH d3d12SDKLayers.dll is
        // servicing the debug layer is the one fact that separates the two
        // ways ID3D12InfoQueue1 can be missing, and it is observable only in
        // a live run -- hence logged at the failure site rather than assumed.
        //
        // Background, because the old WARN here guessed wrong: the vendored
        // Agility redistributable (ThirdParty/AgilitySDK 1.619.3, copied to
        // <exedir>/D3D12/ beside D3D12Core.dll) DOES implement the interface,
        // while the Windows 10 in-box layer (C:\Windows\System32\
        // d3d12SDKLayers.dll, 10.0.19041.x) does not carry it at all -- the
        // IID does not appear anywhere in that binary. So a failed QI means
        // the in-box layer answered, NOT that the D3D12 runtime is
        // "pre-Agility" (NRI logs "Using ID3D12Device15" in the same run,
        // which only the Agility runtime can satisfy).
        std::string LoadedD3D12SDKLayersModule()
        {
            const HMODULE module = GetModuleHandleW(L"d3d12SDKLayers.dll");
            if (!module)
                return "d3d12SDKLayers.dll not loaded";

            wchar_t wide[MAX_PATH]{};
            if (GetModuleFileNameW(module, wide, static_cast<DWORD>(std::size(wide))) == 0)
                return "d3d12SDKLayers.dll loaded, path unavailable";

            char narrow[MAX_PATH * 2]{};
            size_t converted = 0;
            wcstombs_s(&converted, narrow, wide, _TRUNCATE);
            return narrow;
        }

        // NRI capability contract item 12: the D3D12 debug layer's own channel
        // into our log and the RenderErrorCount latch.
        //
        // Why it has to be OURS: NRI never copies enableGraphicsAPIValidation
        // into its internal desc on the wrapper path, so its whole info-queue
        // block -- including ID3D12InfoQueue1::RegisterMessageCallback -- is
        // dead code for us, and its CallbackInterface carries NRI's own
        // messages only. Without this, D3D12 validation text reaches nothing:
        // the block below merely turns break-on-severity off. This is the one
        // channel by which a D3D12 VUID can fail the 0/0 gate, and it is the
        // exact counterpart of DeviceCreationVulkan.cpp's VkDebugCallback --
        // same sink, same severity split, so "an error happened" means one
        // thing on both backends.
        //
        // __stdcall by D3D12MessageFunc's typedef (d3d12sdklayers.h); the
        // calling convention must match exactly.
        void __stdcall D3D12DebugLayerCallback(D3D12_MESSAGE_CATEGORY /*category*/,
                                               D3D12_MESSAGE_SEVERITY severity,
                                               D3D12_MESSAGE_ID /*id*/,
                                               LPCSTR description,
                                               void* /*context*/)
        {
            const char* text = description ? description : "";
            switch (severity)
            {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            case D3D12_MESSAGE_SEVERITY_ERROR:
                // Same latch every other render-layer error producer
                // increments, so a raw D3D12 message fails the GPU tests
                // exactly like an NRI error -- but through NoteError, which
                // tags it "[d3d12]" (this text is the debug layer's, nobody
                // else's) and skips the device-removed substring hook. This
                // callback runs on whatever thread tripped the error, from
                // inside a D3D12 call; ObserveDeviceRemoved above writes a
                // report + minidump, and the producers that may fire it are
                // the latch's own two seams, not this one. See
                // RenderErrorLatch::NoteError for the full argument.
                RenderErrorLatch::Instance().NoteError("d3d12", text);
                break;
            case D3D12_MESSAGE_SEVERITY_WARNING:
                ARC_WARN("[d3d12] {}", text);
                break;
            default:
                // INFO/MESSAGE: the debug layer emits one per resource create
                // and destroy. The Vulkan messenger subscribes to Error and
                // Warning only (DeviceCreationVulkan.cpp) -- match it rather
                // than drown the log.
                break;
            }
        }
    }

    // ----------------------------------------------------------------
    // The CREATION HALF (NRI Phase 1, Task 7).
    // ----------------------------------------------------------------
    // This function WAS the prologue of DeviceD3D12::Init (Phase 1, Task 7
    // moved it out whole so the NRI wrapper could reuse it; Phase 5a, Task 8b
    // deleted the class that kept the other half). It is unchanged by either
    // move: the same calls in the same order with the same parameters, the
    // same log lines, and the same early returns. The member ORDER inside
    // D3D12DeviceCreation still reproduces the COM release order that class
    // had, which is why teardown did not shift either.
    //
    // It sits at namespace scope (outside the anonymous namespace above)
    // because DeviceCreationD3D12.hpp declares it for its consumers -- today
    // the ONE consumer, Nri/NriDevice.cpp's NativeDeviceOwner.
    //
    // Failure leaves `out` holding whatever was created; the caller's teardown
    // releases it, exactly as ~DeviceD3D12 did when Init bailed.
    bool CreateD3D12NativeDevice(const RenderDeviceDesc& desc, D3D12DeviceCreation& out)
    {
        // Recorded, not acted on: NRI's own validation layer is available in
        // wrapper mode (contract 2.1) and keys off the same switch the NVRHI
        // validation layer does. Pure member write -- no call, no branch,
        // nothing the NVRHI path can observe.
        out.enableValidation = desc.enableValidation;

        // The debug layer is PROCESS-GLOBAL state with a one-shot window (see
        // g_d3d12DeviceCreated above): it can only be turned on while this
        // process owns no D3D12 device, and turning it on later removes the
        // device that already exists. So this asks three questions in order --
        // is it already on, is it too late, otherwise turn it on -- rather than
        // enabling unconditionally. `debugLayerActive` is what the rest of this
        // function keys off, because "the caller asked for the layer" and "the
        // layer is servicing this device" stopped being the same thing here.
        UINT factoryFlags     = 0;
        bool debugLayerActive = false;
        if (desc.enableD3D12DebugLayer)
        {
            if (g_d3d12DebugLayerEnabled.load(std::memory_order_acquire))
            {
                // Already on process-wide, from an earlier device's creation.
                // Re-calling EnableDebugLayer is the illegal post-device call
                // AND would buy nothing: the layer that is already loaded is
                // the one that will service this device too.
                debugLayerActive = true;
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            }
            else if (g_d3d12DeviceCreated.load(std::memory_order_acquire))
            {
                // THE SECOND-DEVICE CASE, and the tradeoff stated out loud.
                //
                // NRI PHASE 3, TASK 6 UPDATE: this is no longer the
                // `--nri-graph` case. That path now creates NO NVRHI device at
                // all (GpuContext::CreateForGraph), so the graph's device is
                // the FIRST in the process and takes the else-branch below --
                // it gets the debug layer, which is exactly the "HOW TO GET IT
                // BACK" note further down, arrived at by removing the other
                // device rather than by enabling the layer earlier. This
                // branch stays because the guard is general (any second device
                // that requests the layer) -- the editor's own flip
                // (EditorApp::GraphMode(), collapsed at NRI Phase 5a, Task 11a)
                // is long since complete, so nothing in the tree creates a
                // second D3D12 device today, but the branch remains reachable
                // by anything that does.
                //
                // This device gets NO D3D12 debug layer: enabling it now would
                // remove the engine's live NVRHI device (and, observed at the
                // desk, makes the D3D12CreateDevice below fail outright), so
                // the choice is "one device short of a validation channel" vs
                // "no working device at all".
                //
                // WHAT IS LOST: D3D12 CPU validation messages for THIS device
                // cannot reach D3D12DebugLayerCallback and therefore cannot fail
                // the RenderErrorCount latch. NRI's own validation layer and (on
                // Vulkan) the VK validation layers are unaffected -- they are
                // per-device, not process-global. On the dev box the loss is
                // currently nil: the in-box Win10 D3D12SDKLayers.dll implements
                // no ID3D12InfoQueue1, so those messages reach nothing anyway
                // (see the WARN further down).
                //
                // HOW TO GET IT BACK, when it is worth having: the FIRST device
                // in the process has to be the one that turns the layer on --
                // i.e. the host would set RenderDeviceDesc::enableD3D12DebugLayer
                // on its own boot device when a --nri-graph run is requested,
                // and this branch would then never be reached (the first branch
                // above would take it instead). Deliberately NOT done as part of
                // this fix: it would newly subject the engine's NVRHI half to a
                // debug layer it has never run under, which can only ADD ways
                // for the vehicle run to exit nonzero for reasons that have
                // nothing to do with the graph. Phase 3's one-device flip
                // dissolves the question entirely.
                ARC_WARN("D3D12 debug layer NOT enabled for this device: a D3D12 device already "
                         "exists in this process and EnableDebugLayer is documented to remove it "
                         "when called after device creation. This device's D3D12 validation "
                         "messages will not reach the log or the error latch.");
            }
            else
            {
                ComPtr<ID3D12Debug> debug;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                {
                    debug->EnableDebugLayer();
                    // Set BEFORE the create below, so the flag means "this
                    // process has called EnableDebugLayer" and not "a device
                    // came up afterwards" -- a failed create must not leave the
                    // next caller thinking the layer is still enablable.
                    g_d3d12DebugLayerEnabled.store(true, std::memory_order_release);
                    debugLayerActive = true;
                    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                }
                else
                {
                    ARC_WARN("D3D12 debug layer unavailable; continuing without it");
                }
            }
        }

        if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&out.factory))))
        {
            ARC_ERROR("CreateDXGIFactory2 failed");
            return false;
        }

        if (FAILED(out.factory->EnumAdapterByGpuPreference(
                0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&out.adapter))))
        {
            ARC_ERROR("No DXGI adapter found");
            return false;
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        out.adapter->GetDesc1(&adapterDesc);
        char name[128]{};
        size_t converted = 0;
        wcstombs_s(&converted, name, adapterDesc.Description, _TRUNCATE);
        out.adapterName = name;

        // F-2b: DRED settings are process-global and "you must configure
        // them prior to creating a Direct3D 12 Device" -- modifications
        // have no effect on devices already created. This must therefore
        // sit BEFORE D3D12CreateDevice, and it is deliberately independent
        // of enableD3D12DebugLayer (D3D12GetDebugInterface fetches the DRED
        // settings object without enabling the debug layer). Never fatal:
        // every failure inside degrades the tier and logs one WARN.
        //
        // NRI capability contract item 13: stays exactly here. NRI v180
        // contains no DRED code at all (zero matches for DRED /
        // AutoBreadcrumb / PageFault across its Source and Include), and it
        // never creates the device in wrapper mode, so it cannot clobber
        // this -- but only if the call keeps its position ahead of create.
        EnableD3D12Dred();

        // The HRESULT is in the message because this call's failure mode is
        // otherwise indistinguishable at the desk: D1 hit it three times in a
        // row with no way to tell "no 12_0 adapter" from "the runtime is in a
        // state that refuses to create one" -- SUSPECTED to be an
        // EnableDebugLayer-after-device call (see g_d3d12DeviceCreated), per
        // the MS docs cited above, but never confirmed beyond that one desk
        // repro; treat it as a working theory, not a diagnosed mechanism.
        // That theory also leans on an UNSTATED assumption: the check-then-act
        // read of g_d3d12DebugLayerEnabled/g_d3d12DeviceCreated above is two
        // independent atomic loads, not one transaction, so it is only race-
        // free if CreateD3D12NativeDevice is never entered from more than one
        // thread at a time. Nothing in the tree calls this off the main
        // thread today; a concurrent caller would need its own serialization.
        const HRESULT createHr = D3D12CreateDevice(out.adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                                   IID_PPV_ARGS(&out.device));
        if (FAILED(createHr))
        {
            ARC_ERROR("D3D12CreateDevice failed (feature level 12_0, hr=0x{:08X}) on '{}'",
                      static_cast<uint32_t>(createHr), out.adapterName);
            return false;
        }

        // From here on this process owns a device, so the EnableDebugLayer
        // window above is CLOSED for every later creation.
        g_d3d12DeviceCreated.store(true, std::memory_order_release);

        // The device-side half of the debug layer. BOTH QueryInterface results
        // are kept, because which one fails IS the diagnosis: the base
        // ID3D12InfoQueue is implemented by the debug layer's device wrapper,
        // so failing it means the debug layer is not on this device at all,
        // while failing only ID3D12InfoQueue1 means the layer that answered is
        // too old for the callback interface. The old WARN here could tell
        // neither apart -- it discarded both HRESULTs -- and asserted a cause
        // ("pre-Agility D3D12 runtime") that the same run's "Using
        // ID3D12Device15" disproves. Each branch below now states only what it
        // actually knows.
        //
        // Keyed on `debugLayerActive`, NOT on desc.enableD3D12DebugLayer: when
        // the enable above was skipped because a device already existed, the
        // layer is genuinely absent from this device and every WARN in here
        // would be reporting our own decision back to us as a mystery.
        if (debugLayerActive)
        {
            ComPtr<ID3D12InfoQueue> infoQueue;
            const HRESULT infoQueueHr = out.device.As(&infoQueue);
            if (SUCCEEDED(infoQueueHr))
            {
                // The D3D12 debug layer defaults to break-on-error, which calls
                // __fastfail when the info queue receives a
                // D3D12_MESSAGE_SEVERITY_ERROR or CORRUPTION message. Route all
                // validation through the callback below (which logs at the
                // appropriate level and bumps the latch) rather than aborting
                // the process on first error.
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);

                // Phase 2, Task 1 (Phase 1 Task 6's deferred minor): deny
                // INFO/MESSAGE at the info queue instead of dropping them in
                // the callback. The debug layer emits one of each per resource
                // create and destroy; filtering here means they are never
                // stored and never cross into D3D12DebugLayerCallback at all.
                // Same subscription as the Vulkan messenger, which takes Error
                // and Warning only (DeviceCreationVulkan.cpp) -- so "an error
                // happened" and "the log is quiet" mean one thing on both
                // backends.
                D3D12_MESSAGE_SEVERITY denied[]{ D3D12_MESSAGE_SEVERITY_INFO,
                                                 D3D12_MESSAGE_SEVERITY_MESSAGE };
                D3D12_INFO_QUEUE_FILTER filter{};
                filter.DenyList.NumSeverities = static_cast<UINT>(std::size(denied));
                filter.DenyList.pSeverityList = denied;
                if (FAILED(infoQueue->PushStorageFilter(&filter)))
                {
                    ARC_WARN("ID3D12InfoQueue::PushStorageFilter failed; D3D12 INFO/MESSAGE "
                             "chatter will reach the debug-layer callback");
                }
            }

            // NRI capability contract item 12: turning break-off is all the
            // block above ever did -- the messages themselves went nowhere.
            // ID3D12InfoQueue1::RegisterMessageCallback is what actually
            // delivers them (see D3D12DebugLayerCallback). Missing it is a
            // diagnostics degradation, never a create failure.
            ComPtr<ID3D12InfoQueue1> infoQueue1;
            const HRESULT infoQueue1Hr = out.device.As(&infoQueue1);
            if (SUCCEEDED(infoQueue1Hr))
            {
                DWORD         cookie     = 0;
                const HRESULT registerHr = infoQueue1->RegisterMessageCallback(
                    &D3D12DebugLayerCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
                if (SUCCEEDED(registerHr))
                {
                    out.infoQueue       = infoQueue1;
                    out.infoQueueCookie = cookie;
                }
                else
                {
                    ARC_WARN("ID3D12InfoQueue1::RegisterMessageCallback failed (hr=0x{:08X}); "
                             "D3D12 debug-layer messages will not reach the log",
                             static_cast<uint32_t>(registerHr));
                }
            }
            else if (FAILED(infoQueueHr))
            {
                // TRUE failure case 1: no info queue of any generation, i.e.
                // the debug layer is not attached to this device. Either
                // EnableDebugLayer above did not take effect for the runtime
                // that created the device, or the device predates the enable.
                ARC_WARN("the D3D12 debug layer is not active on this device (ID3D12InfoQueue "
                         "QueryInterface failed, hr=0x{:08X}); D3D12 debug-layer messages will "
                         "not reach the log",
                         static_cast<uint32_t>(infoQueueHr));
            }
            else
            {
                // TRUE failure case 2: the debug layer IS attached (the base
                // interface resolved) but the SDK layers servicing it predate
                // ID3D12InfoQueue1. The module path is the actionable half --
                // <exedir>/D3D12/ is the vendored Agility layer, which has the
                // interface; System32 is the Windows 10 in-box layer, which
                // does not carry the IID at all.
                ARC_WARN("the loaded D3D12 debug layer does not implement ID3D12InfoQueue1 "
                         "(hr=0x{:08X}); D3D12 debug-layer messages will not reach the log. "
                         "Layer servicing this device: {}",
                         static_cast<uint32_t>(infoQueue1Hr), LoadedD3D12SDKLayersModule());
            }
        }

        // NRI capability contract item 10 (creation half): this ONE direct
        // queue is what the wrapper desc must carry in
        // QueueFamilyD3D12Desc::d3d12Queues -- leaving that null makes NRI
        // create its own, and the DXGI swapchain below is bound to THIS
        // one, so it would be presenting on a queue NRI never submits to.
        // Reachable for the wrap through GraphicsQueue() above.
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(out.device->CreateCommandQueue(&queueDesc,
                                                 IID_PPV_ARGS(&out.graphicsQueue))))
        {
            ARC_ERROR("CreateCommandQueue failed");
            return false;
        }

        return true;
    }

    // Contract item 12's teardown half, idempotent: the debug layer holds a
    // raw pointer to a function in THIS module and must not outlive it. Split
    // out of the release below because the NVRHI path had to unregister at one
    // specific point -- before its nvrhi device was released, which is exactly
    // where ~DeviceD3D12 called it. That caller is gone as of Phase 5a Task 8b;
    // the split stays because DestroyD3D12NativeDevice below is written on top
    // of it and idempotence makes the pair safe either way.
    void UnregisterD3D12DebugCallback(D3D12DeviceCreation& creation)
    {
        if (creation.infoQueue && creation.infoQueueCookie != 0)
        {
            creation.infoQueue->UnregisterMessageCallback(creation.infoQueueCookie);
            creation.infoQueueCookie = 0;
        }
    }

    // Owner teardown (contract item 15: the NRI device is destroyed BEFORE
    // this runs). Releases in the order D3D12DeviceCreation's member layout
    // encodes -- queue, info queue, device, adapter, factory -- which is the
    // order ~DeviceD3D12's member destruction has always produced.
    void DestroyD3D12NativeDevice(D3D12DeviceCreation& creation)
    {
        UnregisterD3D12DebugCallback(creation);
        creation.graphicsQueue.Reset();
        creation.infoQueue.Reset();
        creation.device.Reset();
        creation.adapter.Reset();
        creation.factory.Reset();
    }

    // The narrow export (DeviceRemovedObservers.hpp, NRI Phase 3 Task 5): the
    // SAME observer above, reachable BY ADDRESS from the Render module's
    // installer. One line, no state, no second observation point -- the
    // once-only `g_deviceRemovedReported` latch, the "gpu-crash: device
    // removed" wording and the NoteGpuDeviceLost ordering all stay in
    // ObserveDeviceRemoved, unchanged and file-local.
    //
    // IT STAYS A SEPARATE FUNCTION FROM THE OBSERVER (Task 8b). Folding the
    // body up into this name would be a behaviour change, not a cleanup:
    // NriDiagnostics::Disarm clears the hook slot only when it still holds
    // the address Arm installed, and that address is THIS function's.
    void ObserveDeviceRemovedD3D12()
    {
        ObserveDeviceRemoved();
    }

    // Its twin (DeviceRemovedObservers.hpp): the store DeviceD3D12::Init used
    // to make one line above its own ResetGpuDeviceLost(). Since Task 8b
    // deleted that class, NriDiagnostics::Arm is the ONLY arming site left and
    // this is its only way to reach a latch that is deliberately file-local.
    void ResetDeviceRemovedLatchD3D12()
    {
        g_deviceRemovedReported.store(false, std::memory_order_release);
    }

    // The device-loss QUESTION, as opposed to the observers above which are
    // the ANSWER's delivery. See DeviceRemovedObservers.hpp for why NRI's
    // D3D12 QueueWaitIdle cannot be asked instead.
    //
    // GetDeviceRemovedReason is the one D3D12 call that is defined ON a
    // removed device. S_OK means "not removed"; every other HRESULT
    // (DXGI_ERROR_DEVICE_REMOVED / _HUNG / _RESET,
    // DXGI_ERROR_DRIVER_INTERNAL_ERROR) means it is gone. Deliberately not
    // once-only and deliberately silent: it observes nothing and reports
    // nothing, so it stays safe to call from a bail-out path.
    bool D3D12NativeDeviceRemoved(void* nativeDevice) noexcept
    {
        if (!nativeDevice)
            return false;
        return FAILED(static_cast<ID3D12Device*>(nativeDevice)->GetDeviceRemovedReason());
    }
}
