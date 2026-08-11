# GPU crash diagnostics -- seam facts F-1..F-8

Task 1 of `docs/plans/2026-08-11-gpu-crash-diagnostics.md`. This file discharges
the plan's deliberately-deferred API verification debt. Tasks 5-7 bind code to
these facts VERBATIM -- if a fact here is wrong, the implementation is wrong.

Every in-repo fact carries `path:line`. Every SDK fact carries the header path on
THIS machine plus the exact symbol. Every web fact carries a URL. Line numbers
were re-opened and re-read after drafting (self-review pass).

Survey date: 2026-08-11. Repo: `D:\dev\starworks\Arcane` @ `main`.

## Step 1 -- reference availability

`Test-Path D:\dev\starworks\Arcane\.example` => **False**. The Unreal Engine 5.6
reference tree was deleted from disk in the 2026-08-11 cleanup and is NOT
available for this survey. F-1/F-2/F-5 therefore cite Microsoft documentation,
the DirectX-Specs DRED spec, the Windows SDK / vendored DirectX-Headers on this
machine, and the vendored Vulkan-Headers -- not UE source.

Windows SDKs installed: `10.0.19041.0`, `10.0.22621.0`, `10.0.26100.0` (under
`C:\Program Files (x86)\Windows Kits\10\Include\`). Premake sets
`systemversion "latest"` (`premake5.lua:145` and siblings), so the VS18 build
targets **10.0.26100.0**. VS18 toolset on this box: MSVC `14.51.36231` under
`C:\Program Files\Microsoft Visual Studio\18\Community`.

**Critical resolution fact (affects F-1 and F-2):** ArcaneClient does NOT compile
against the Windows SDK `d3d12.h`. `premake5.lua:203-204` (inside
`project "ArcaneClient"`, opened at `premake5.lua:174`) puts
`ThirdParty/DirectX-Headers/include` and `.../include/directx` on the include
path, so the `#include <d3d12.h>` at
`ArcaneClient/src/Arcane/Render/DeviceD3D12.cpp:13` resolves to the **vendored**
`ThirdParty/DirectX-Headers/include/directx/d3d12.h`. Verified empirically with
`cl /showIncludes` over a probe TU carrying the same two `/I` paths:

```
Note: including file: D:\dev\starworks\Arcane\ThirdParty\DirectX-Headers\include\directx\d3d12.h
```

The same ordering appears in the generated
`ArcaneClient/ArcaneClient.vcxproj` `AdditionalIncludeDirectories`. NVRHI reaches
the same header explicitly: `ThirdParty/nvrhi/include/nvrhi/d3d12.h:31` is
`#include <directx/d3d12.h>`.

Vendored header version: `D3D12_SDK_VERSION ( 619 )`
(`ThirdParty/DirectX-Headers/include/directx/d3d12.h:1368`), i.e. Agility SDK
1.619. Both headers carry the full DRED surface (see F-2), so this does not
change the API answer -- but Task 5 must cite/bind against the **vendored**
header, because that is what compiles.

---

## F-1: D3D12 marker-buffer recipe

### F-1a -- heap that stays CPU-readable after device removal

**Answer.** Do NOT use `D3D12_HEAP_TYPE_READBACK` or an ordinary
`CreateCommittedResource` heap: those are device-owned and are not guaranteed
readable once the device is removed. Use the purpose-built diagnostic path:

1. `VirtualAlloc` a system-memory region (process-owned, survives device
   removal because the D3D12 device never owned it).
2. Wrap it with **`ID3D12Device3::OpenExistingHeapFromAddress(pAddress, riid,
   ppvHeap)`** -- Windows SDK `10.0.26100.0\um\d3d12.h:11368-11371`; vendored
   `ThirdParty/DirectX-Headers/include/directx/d3d12.h:12139`.
3. `ID3D12Device::CreatePlacedResource` a buffer on that heap
   (SDK `d3d12.h:9136`), initial state `D3D12_RESOURCE_STATE_COPY_DEST` (see
   F-1b -- `WriteBufferImmediate` requires it).
4. Read the marker values through the ORIGINAL `VirtualAlloc` pointer after
   device removal. Never through a `Map()`ed pointer on a device-owned heap.

Microsoft's own one-line description of the API is the fact:

> "Creates a special-purpose diagnostic heap in system memory from an address.
> The created heap can persist even in the event of a GPU-fault or
> device-removed scenario."
> ... "The heap is created in system memory and permits CPU access. It wraps the
> entire VirtualAlloc region."

Source: <https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device3-openexistingheapfromaddress>

**Required capability gate.** `OpenExistingHeapFromAddress` requires driver
support for existing heaps. Check before use:

- `D3D12_FEATURE_EXISTING_HEAPS = 22` -- SDK `d3d12.h:2346`;
  vendored `directx/d3d12.h:2607`.
- `D3D12_FEATURE_DATA_EXISTING_HEAPS { _Out_ BOOL Supported; }` -- SDK
  `d3d12.h:2692-2695`.

Call `ID3D12Device::CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &data,
sizeof(data))` and require `data.Supported != FALSE`. The DRED documentation
names this exact dependency for the same reason (breadcrumbs must outlive the
device):

> "Because the breadcrumb counter values must be preserved after device removal,
> the resource that contains breadcrumbs must exist in system memory, and it must
> persist in the event of device removal. This means that the display driver
> needs to support D3D12_FEATURE_EXISTING_HEAPS. Fortunately, this is the case
> for most Direct3D 12 display drivers on Windows 10, version 1903."

Source: <https://learn.microsoft.com/en-us/windows/win32/direct3d12/use-dred>

**Fallback if `Supported == FALSE`** (record, do not silently skip): a custom
heap in L0 system memory -- `D3D12_HEAP_TYPE_CUSTOM = 4` (SDK `d3d12.h:2978`),
`D3D12_CPU_PAGE_PROPERTY_WRITE_BACK = 3` (`d3d12.h:2988`),
`D3D12_MEMORY_POOL_L0 = 1` (`d3d12.h:2995`), heap flags
`D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS = 0xc0` (`d3d12.h:3025`). This puts the
bytes in system RAM, but Microsoft makes NO persistence-after-removal guarantee
for it -- treat it as best-effort and mark the report accordingly. Task 5 should
prefer refusing markers (log + continue without them) over shipping a recipe
that reads freed memory.

### F-1b -- `WriteBufferImmediate` mode semantics

Signature -- `ID3D12GraphicsCommandList2::WriteBufferImmediate`, SDK
`d3d12.h:7985-7988`:

```cpp
virtual void STDMETHODCALLTYPE WriteBufferImmediate(
    UINT Count,
    _In_reads_(Count)     const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *pParams,
    _In_reads_opt_(Count) const D3D12_WRITEBUFFERIMMEDIATE_MODE      *pModes) = 0;
```

Parameter struct -- SDK `d3d12.h:7951-7955`:

```cpp
typedef struct D3D12_WRITEBUFFERIMMEDIATE_PARAMETER {
    D3D12_GPU_VIRTUAL_ADDRESS Dest;   // GPU VA, not a CPU pointer
    UINT32 Value;                     // exactly 32 bits per write
} D3D12_WRITEBUFFERIMMEDIATE_PARAMETER;
```

Mode enum -- SDK `d3d12.h:7958-7963`; vendored `directx/d3d12.h:8401-8403`:

| Constant | Value | Documented semantics (verbatim) |
|---|---|---|
| `D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT` | `0` | "The write operation behaves the same as normal copy-write operations." |
| `D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN` | `0x1` | "The write operation is guaranteed to occur after all preceding commands in the command stream have started, including previous WriteBufferImmediate operations." |
| `D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT` | `0x2` | "The write operation is deferred until all previous commands in the command stream have completed through the GPU pipeline, including previous WriteBufferImmediate operations. Write operations that specify D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT don't block subsequent operations from starting. If there are no previous operations in the command stream, then the write operation behaves as if D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN was specified." |

Source: <https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_writebufferimmediate_mode>

**Binding rule for Task 7's begin/end scope markers:**

- **Scope BEGIN marker -> `MARKER_IN`.** It lands once the preceding work has
  *started*, so "begin(N) written, end(N) missing" means the GPU entered pass N
  and did not leave it.
- **Scope END marker -> `MARKER_OUT`.** It is deferred until all prior commands
  have *completed through the pipeline*, so its presence is proof the pass
  finished. `MARKER_OUT` does not stall subsequent work.
- **`DEFAULT` is wrong for markers** -- it carries no ordering guarantee relative
  to GPU execution and would produce a marker that says nothing about progress.

**Hard requirement on the destination resource:**

> "The receiving buffer (resource) must be in the D3D12_RESOURCE_STATE_COPY_DEST
> state to be a valid destination for WriteBufferImmediate."

Source: <https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist2-writebufferimmediate>

Create the placed buffer with `D3D12_RESOURCE_STATE_COPY_DEST` as its initial
state and never transition it -- there is no other consumer.

`pModes` may be null; null means every write uses `DEFAULT`. Task 7 must always
pass an explicit `pModes` array.

---

## F-2: DRED enablement -- interfaces, enums, and whether a lightweight tier exists

### F-2a -- what this machine's headers actually contain

Both the Windows SDK `10.0.26100.0` header and the vendored DirectX-Headers
carry the **complete DRED 1.3 surface**. Line numbers for both:

| Symbol | SDK `10.0.26100.0\um\d3d12.h` | Vendored `ThirdParty/DirectX-Headers/include/directx/d3d12.h` |
|---|---|---|
| `ID3D12DeviceRemovedExtendedDataSettings` (IID `82BC481C-6B9B-4030-AEDB-7EE3D1DF1E63`) | 16060-16073 | 17055-17068 |
| `SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT)` | 16064-16065 | 17059-17060 |
| `SetPageFaultEnablement(D3D12_DRED_ENABLEMENT)` | 16067-16068 | 17062-17063 |
| `SetWatsonDumpEnablement(D3D12_DRED_ENABLEMENT)` | 16070-16071 | 17065-17066 |
| `ID3D12DeviceRemovedExtendedDataSettings1` (IID `DBD5AE51-3317-4F0A-ADF9-1D7CEDCAAE0B`) | 16165 | 17160 |
| `SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT)` | 16169 | 17164 |
| `ID3D12DeviceRemovedExtendedDataSettings2` (IID `61552388-01ab-4008-a436-83db189566ea`) | 16273-16280 | 17268-17275 |
| `UseMarkersOnlyAutoBreadcrumbs(BOOL MarkersOnly)` | 16277-16278 | 17272-17273 |
| `ID3D12DeviceRemovedExtendedData` (IID `98931D33-5AE8-4791-AA3C-1A73A2934E71`) | 16390-16400 | 17385 |
| `GetAutoBreadcrumbsOutput(D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT*)` | 16394-16395 | 17389 |
| `GetPageFaultAllocationOutput(D3D12_DRED_PAGE_FAULT_OUTPUT*)` | 16397-16398 | 17392 |
| `ID3D12DeviceRemovedExtendedData1` (IID `9727A022-CF1D-4DDA-9EBA-EFFA653FC506`) | 16484-16493 | 17479 |
| `GetAutoBreadcrumbsOutput1(D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1*)` | 16488-16489 | 17483 |
| `GetPageFaultAllocationOutput1(D3D12_DRED_PAGE_FAULT_OUTPUT1*)` | 16491-16492 | 17486 |
| `ID3D12DeviceRemovedExtendedData2` (IID `67FC5816-E4CA-4915-BF18-42541272DA54`) | 16595-16604 | 17590-17599 |
| `GetPageFaultAllocationOutput2(D3D12_DRED_PAGE_FAULT_OUTPUT2*)` | 16599-16600 | 17594-17595 |
| `GetDeviceState()` -> `D3D12_DRED_DEVICE_STATE` | 16602 | 17597 |

Enums (SDK line -> vendored line):

```cpp
enum D3D12_DRED_ENABLEMENT {              // SDK 15898-15903, vendored 16893-16898
    D3D12_DRED_ENABLEMENT_SYSTEM_CONTROLLED = 0,   // default
    D3D12_DRED_ENABLEMENT_FORCED_OFF        = 1,
    D3D12_DRED_ENABLEMENT_FORCED_ON         = 2
};

enum D3D12_DRED_VERSION {                 // SDK 15880-15886
    D3D12_DRED_VERSION_1_0 = 0x1,
    D3D12_DRED_VERSION_1_1 = 0x2,
    D3D12_DRED_VERSION_1_2 = 0x3,
    D3D12_DRED_VERSION_1_3 = 0x4          // vendored directx/d3d12.h:16880
};

enum D3D12_DRED_DEVICE_STATE {            // SDK 15994-16000
    D3D12_DRED_DEVICE_STATE_UNKNOWN   = 0,
    D3D12_DRED_DEVICE_STATE_HUNG      = 3,
    D3D12_DRED_DEVICE_STATE_FAULT     = 6,
    D3D12_DRED_DEVICE_STATE_PAGEFAULT = 7
};

enum D3D12_DRED_FLAGS {                   // SDK 15889-15894
    D3D12_DRED_FLAG_NONE                  = 0,
    D3D12_DRED_FLAG_FORCE_ENABLE          = 1,
    D3D12_DRED_FLAG_DISABLE_AUTOBREADCRUMBS = 2
};
```

Note `D3D12_DRED_FLAGS` is the DRED 1.0-era mechanism carried by
`D3D12_DEVICE_REMOVED_EXTENDED_DATA` (SDK `d3d12.h:15905-15909`, `Flags` field at
`:15907`); it is NOT the modern enablement path. Use `D3D12_DRED_ENABLEMENT` via
the Settings interfaces.

### F-2b -- full DRED enablement (the answer the brief asks for)

DRED settings are **process-global and must be set BEFORE `D3D12CreateDevice`**:

> "DRED settings are global to the process, and you must configure them prior to
> creating a Direct3D 12 Device."
> ... "Modifications to DRED settings have no effect on devices already created.
> But subsequent calls to D3D12CreateDevice use the most recent DRED settings."

Source: <https://learn.microsoft.com/en-us/windows/win32/direct3d12/use-dred>

Full DRED (auto-breadcrumbs + page fault) is:

```cpp
ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
{
    dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    // optional, Settings1: PIX marker/event strings in breadcrumbs (DRED 1.2)
    // dred1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
}
```

Retrieval after removal is a `QueryInterface` on the **device**, not on the debug
interface:

> "After device removal has been detected (for example, Present returns
> DXGI_ERROR_DEVICE_REMOVED), use the methods of the
> ID3D12DeviceRemovedExtendedData interface to access the DRED data for the
> removed device. To retrieve the ID3D12DeviceRemovedExtendedData interface,
> call QueryInterface on an ID3D12Device (or derived) interface..."

Same source.

### F-2c -- IS there a lightweight tier? (the brief's decision question)

**YES -- and it is not `D3D12_DRED_ENABLEMENT`.** The lightweight mode is
`ID3D12DeviceRemovedExtendedDataSettings2::UseMarkersOnlyAutoBreadcrumbs(BOOL)`,
present in BOTH headers on this machine (SDK `d3d12.h:16277`, vendored
`directx/d3d12.h:17272`). DirectX-Specs describes it as:

> "Suppresses DRED auto-breadcrumbs for all operations except:
> ID3D12GraphicsCommandList::SetMarker, ID3D12GraphicsCommandList::BeginEvent,
> ID3D12GraphicsCommandList::EndEvent"

Source: <https://microsoft.github.io/DirectX-Specs/d3d/DeviceRemovedExtendedData.html>

That is precisely the "cheap tier": instead of a breadcrumb after every render op
(Draw/Dispatch/Copy/Resolve/...), DRED only writes breadcrumbs at PIX
marker/event boundaries -- which is exactly the granularity Task 7's pass-scope
instrumentation produces anyway. The overhead being avoided is documented:

> "Although auto-breadcrumbs are designed to be low-overhead, they are not free.
> Empirical measurements show 2-5% performance loss on a typical AAA Direct3D 12
> graphics game engine. For this reason, auto-breadcrumbs are off by default."

Source: <https://learn.microsoft.com/en-us/windows/win32/direct3d12/use-dred>
(page-fault reporting is separately documented as "increases the system memory
overhead, and introduces a small performance hit to object creation and
destruction", also off by default).

**Consequence for the spec's Dist row: the spec's table does NOT need the
"breadcrumbs-only fallback" amendment the brief anticipated.** A Dist build can
run:

```
SetAutoBreadcrumbsEnablement(FORCED_ON)
SetPageFaultEnablement(FORCED_OFF)      // or SYSTEM_CONTROLLED
settings2->UseMarkersOnlyAutoBreadcrumbs(TRUE)
```

i.e. genuine lightweight DRED, not full DRED and not nothing.

### F-2d -- UNVERIFIED (needs desk check): runtime availability of DRED 1.3

The headers have `Settings2`/`DRED2`; the **runtime** does not necessarily.
Evidence:

- The repo has **no Agility SDK opt-in**: grep for `D3D12SDKVersion`,
  `D3D12SDKPath`, `D3D12Core` across all non-`ThirdParty` `.cpp`/`.hpp`/`.lua`
  returns nothing, and no `D3D12Core.dll` is shipped anywhere in the tree. The
  process therefore binds the OS inbox `d3d12.dll`.
- This machine is Windows 10 Pro 19045 (22H2). DRED 1.2 is documented as
  available from Windows 10 1903-era runtimes; DRED 1.3
  (`UseMarkersOnlyAutoBreadcrumbs`, `GetDeviceState`,
  `GetPageFaultAllocationOutput2`) is newer and I could not confirm from docs
  which OS build first exposes it, and I am not permitted to run a GPU host on
  this box to probe it.

**Best-supported candidate answer + mandatory mitigation for Task 5:** assume
`Settings2`/`DRED2` may `QueryInterface`-fail at runtime and implement a
descending ladder, taking the first that succeeds and logging which tier was
obtained:

- Enablement: `Settings2` -> `Settings1` -> `Settings`. Only call
  `UseMarkersOnlyAutoBreadcrumbs` on the `Settings2` rung; only call
  `SetBreadcrumbContextEnablement` on `Settings1`+.
- Retrieval: `ID3D12DeviceRemovedExtendedData2` -> `...Data1` -> `...Data`.
  Only call `GetDeviceState` / `GetPageFaultAllocationOutput2` on the `Data2`
  rung, `GetAutoBreadcrumbsOutput1` / `GetPageFaultAllocationOutput1` on
  `Data1`+.

If the `Settings2` rung is unavailable at runtime, the Dist row degrades to
full auto-breadcrumbs (or breadcrumbs-off) -- and THAT is when the spec's table
needs the noted amendment. Record the achieved tier in the crash report so the
question answers itself the first time a real report lands.

---

## F-3: where device-removed becomes observable in Arcane

### F-3a -- there is only one message sink, and it is header-only

`ArcaneClient/src/Arcane/Render/NvrhiMessageCallback.hpp` (50 lines). **There is
no `NvrhiMessageCallback.cpp`** -- the brief's `{hpp,cpp}` is a header-only
class; do not go looking for the `.cpp`.

- `class NvrhiMessageCallback final : public nvrhi::IMessageCallback` -- line 15.
- `static NvrhiMessageCallback& Instance()` (function-local static singleton) --
  lines 18-22.
- `void message(nvrhi::MessageSeverity severity, const char* messageText)
  override` -- line 26. The **error path** is lines 36-39
  (`case nvrhi::MessageSeverity::Error: ++m_errorCount; ARC_ERROR("[nvrhi] {}",
  messageText); break;`) and lines 40-43 for `Fatal`.
- `uint64_t ErrorCount() const` -- line 24; exposed engine-wide as
  `Arcane::RenderErrorCount()` (`Device.hpp:31`, implemented
  `Device.cpp:9-12`).

It is installed on both backends: `DeviceD3D12.cpp:129`
(`nvrhiDesc.errorCB = &NvrhiMessageCallback::Instance();`) and
`DeviceVulkan.cpp:635` (same line for the Vulkan `DeviceDesc`). Vulkan's own
validation callback also funnels errors into it --
`DeviceVulkan.cpp:63-65`.

### F-3b -- the cross-backend earliest observable (the primary Task 5/6 hook)

**NVRHI itself detects device removal on submit, on BOTH backends, and reports it
through the message callback with the literal string `"Device Removed!"`.**

- D3D12: `ThirdParty/nvrhi/src/d3d12/d3d12-device.cpp:627-631` --
  `Device::executeCommandLists` calls
  `m_Context.device->GetDeviceRemovedReason()` after every
  `ExecuteCommandLists` + `Signal`, and on `FAILED(hr)` calls
  `m_Context.messageCallback->message(MessageSeverity::Error, "Device Removed!")`.
- Vulkan: `ThirdParty/nvrhi/src/vulkan/vulkan-queue.cpp:195-201` --
  `Queue::submit` wraps `m_Queue.submit(submitInfo)` in
  `try { ... } catch (vk::DeviceLostError&) { m_Context.messageCallback->message(
  MessageSeverity::Error, "Device Removed!"); }`.
- Also Vulkan: `ThirdParty/nvrhi/src/vulkan/vulkan-device.cpp:315-324` --
  `Device::waitForIdle` swallows `vk::DeviceLostError` and returns `false`
  (silent; not a message-callback site).

**=> `NvrhiMessageCallback::message` (NvrhiMessageCallback.hpp:26, error branch
lines 36-39) is THE function Task 5/6 hook.** It is the single point that both
backends reach, it is already the process-wide sink, and it fires on submit --
i.e. before the next Present. Task 5/6 should trigger capture from here when
`severity == Error` and the text matches NVRHI's device-removal report, then
delegate to the backend-specific collector.

### F-3c -- the D3D12 present-path observable (the only first-party site today)

`ArcaneClient/src/Arcane/Render/DeviceD3D12.cpp:274-297` --
`SwapchainD3D12::Present()`:

```cpp
const HRESULT hr = m_swapchain->Present(m_vsync ? 1 : 0, 0);          // :279
if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) // :280
{
    ARC_ERROR("Present failed: device removed/reset (0x{:08X}), reason 0x{:08X}",
              (uint32_t)hr,
              (uint32_t)m_device->D3D12Device()->GetDeviceRemovedReason());   // :282-284
}
```

This is the ONLY `DXGI_ERROR_DEVICE_REMOVED` / `GetDeviceRemovedReason` site in
the entire first-party tree -- verified by a repo-wide grep for
`DEVICE_REMOVED|DEVICE_RESET|DeviceLost|DEVICE_LOST|GetDeviceRemovedReason` over
`*.cpp`/`*.hpp` excluding `ThirdParty/`, which returns exactly
`DeviceD3D12.cpp:280` and `:284`. Task 5 replaces this log-and-continue with the
DRED collection + report path.

### F-3d -- Vulkan has NO first-party device-lost handling today

`SwapchainVulkan::Present()` (`DeviceVulkan.cpp:366-407`) calls
`m_device->GraphicsQueue().presentKHR(presentInfo)` at line 392 inside a
`try { } catch (const vk::OutOfDateKHRError&)` (lines 390-397) -- **only**
`OutOfDateKHRError` is caught. A `vk::DeviceLostError` from `presentKHR` would
propagate out of `Present()` as an unhandled C++ exception. Same shape at
`BeginFrame`'s `acquireNextImageKHR` (`DeviceVulkan.cpp:338-358`).

Task 6 must add a `vk::DeviceLostError` catch at both sites
(`DeviceVulkan.cpp:390-397` and `:338-358`) in addition to hooking F-3b.
(Vulkan-Hpp maps `VK_ERROR_DEVICE_LOST` to `vk::DeviceLostError`; NVRHI's own
string table confirms the code at
`ThirdParty/nvrhi/src/vulkan/vulkan-constants.cpp:417-418`.)

### F-3e -- Aftermath is present in the vendored NVRHI but OFF, and stays off

`ThirdParty/nvrhi/include/nvrhi/common/aftermath.h` exists, `nvrhi::IDevice`
declares `virtual bool isAftermathEnabled() = 0` (`nvrhi.h:3795`, with
`getAftermathCrashDumpHelper()` at `:3796`), and both
backend `DeviceDesc`s carry `bool aftermathEnabled = false;`
(`ThirdParty/nvrhi/include/nvrhi/d3d12.h:126`,
`ThirdParty/nvrhi/include/nvrhi/vulkan.h:78`). Arcane never sets either field
(`DeviceD3D12.cpp:128-132`, `DeviceVulkan.cpp:634-644` set only `errorCB` and the
handles), and no `.lua` in the first-party tree mentions aftermath. The plan's
NO-Aftermath constraint is already the status quo -- Task 5/6 must not flip
`aftermathEnabled`.

---

## F-4: reaching the native D3D12/Vulkan device, queue, and command list

### F-4a -- NVRHI's accessor seams

- `virtual Object getNativeObject(ObjectType objectType)` --
  `ThirdParty/nvrhi/include/nvrhi/common/resource.h:118`. Declared on
  `IResource` (line 105), so **every** NVRHI object has it:
  `class IDevice : public IResource` (`nvrhi.h:3671`),
  `class ICommandList : public IResource` (`nvrhi.h:3182`),
  `class IBuffer : public IResource` (`nvrhi.h:758`),
  `class ITexture : public IResource` (`nvrhi.h:543`).
  Doc comment at `resource.h:116-117`: "Returns a native object or interface ...
  or nullptr if the requested interface is unavailable. Does *not* AddRef the
  returned interface."
- `virtual Object getNativeQueue(ObjectType objectType, CommandQueue queue) = 0`
  -- `ThirdParty/nvrhi/include/nvrhi/nvrhi.h:3791` (on `IDevice`). This is the
  ONLY way to reach a queue; queues are not `IResource`s.
- `typedef uint32_t ObjectType;` -- `common/resource.h:31`.

### F-4b -- the exact object-type constants

`namespace nvrhi::ObjectTypes` in `ThirdParty/nvrhi/include/nvrhi/common/resource.h:42`:

| Constant | Value | Line | Native type |
|---|---|---|---|
| `D3D12_Device` | `0x00020001` | `resource.h:55` | `ID3D12Device*` |
| `D3D12_CommandQueue` | `0x00020002` | `resource.h:56` | `ID3D12CommandQueue*` |
| `D3D12_GraphicsCommandList` | `0x00020003` | `resource.h:57` | `ID3D12GraphicsCommandList*` |
| `D3D12_Resource` | `0x00020004` | `resource.h:58` | `ID3D12Resource*` |
| `D3D12_CommandAllocator` | `0x0002000b` | `resource.h:65` | `ID3D12CommandAllocator*` |
| `VK_Device` | `0x00030001` | `resource.h:67` | `VkDevice` |
| `VK_PhysicalDevice` | `0x00030002` | `resource.h:68` | `VkPhysicalDevice` |
| `VK_Instance` | `0x00030003` | `resource.h:69` | `VkInstance` |
| `VK_Queue` | `0x00030004` | `resource.h:70` | `VkQueue` |
| `VK_CommandBuffer` | `0x00030005` | `resource.h:71` | `VkCommandBuffer` |
| `VK_DeviceMemory` | `0x00030006` | `resource.h:72` | `VkDeviceMemory` |
| `VK_Buffer` | `0x00030007` | `resource.h:73` | `VkBuffer` |
| `VK_Image` | `0x00030008` | `resource.h:74` | `VkImage` |

Backend-specific extras (the NVRHI wrapper objects, not the native API objects):
`Nvrhi_D3D12_Device = 0x00020101`, `Nvrhi_D3D12_CommandList = 0x00020102`
(`ThirdParty/nvrhi/include/nvrhi/d3d12.h:37-38`); `Nvrhi_VK_Device = 0x00030101`
(`ThirdParty/nvrhi/include/nvrhi/vulkan.h:32`).

**Note the D3D12 command-list gap:** NVRHI exposes
`D3D12_GraphicsCommandList` (base `ID3D12GraphicsCommandList`), NOT
`ID3D12GraphicsCommandList2`. Task 7 must `QueryInterface` up:
`cmd->getNativeObject(ObjectTypes::D3D12_GraphicsCommandList)` ->
`ID3D12GraphicsCommandList*` -> `QueryInterface(IID_PPV_ARGS(&list2))` for
`WriteBufferImmediate`. Handle a null/failed QI (log once, disable markers).

### F-4c -- the shorter first-party route (preferred where available)

Arcane already owns the native D3D12 handles directly and does not need
`getNativeObject` for device/queue on that backend:

- `ID3D12Device* DeviceD3D12::D3D12Device() const` --
  `ArcaneClient/src/Arcane/Render/DeviceD3D12.cpp:42`.
- `ID3D12CommandQueue* DeviceD3D12::GraphicsQueue() const` --
  `DeviceD3D12.cpp:41`.
- `IDXGIFactory6* DeviceD3D12::Factory() const` -- `DeviceD3D12.cpp:40`.

**But `DeviceD3D12` is declared inside an anonymous namespace**
(`DeviceD3D12.cpp:25-31`), so those accessors are TU-local -- only reachable from
`DeviceD3D12.cpp` itself. The public `RenderDevice` interface
(`Device.hpp:50-69`) exposes only `Backend()`, `Nvrhi()`, `AdapterName()`,
`CreateSwapchain()`. Task 5 therefore either (a) does its D3D12 work inside
`DeviceD3D12.cpp`, or (b) goes through
`device.Nvrhi()->getNativeObject(nvrhi::ObjectTypes::D3D12_Device)`. Same story
on Vulkan: `DeviceVulkan::Instance()/PhysicalDevice()/Device()/GraphicsQueue()/
GraphicsQueueFamily()` at `DeviceVulkan.cpp:81-85` are anonymous-namespace-local.

---

## F-5: Vulkan enablement path (`VK_EXT_device_fault`, `VK_AMD_buffer_marker`)

### F-5a -- where the extension lists live today

`ArcaneClient/src/Arcane/Render/DeviceVulkan.cpp`:

```cpp
const char* kInstanceExtensions[] = {          // :38-41
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
};
const char* kDeviceExtensions[] = {            // :42-44
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};
```

### F-5b -- how availability is queried TODAY (instance level only)

Instance extensions ARE filtered by availability, but only for one extension.
`DeviceVulkan.cpp:490-505`:

```cpp
std::vector<const char*> instanceExtensions(
    std::begin(kInstanceExtensions), std::end(kInstanceExtensions));   // :490-491
bool debugUtils = false;                                              // :492
if (desc.enableValidation)                                            // :493
{
    for (const auto& ext : vk::enumerateInstanceExtensionProperties()) // :495
    {
        if (std::string_view(ext.extensionName) ==
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME)                        // :497-498
        { instanceExtensions.push_back(...); debugUtils = true; break; } // :500-502
    }
}
```

Layers are filtered the same way via `vk::enumerateInstanceLayerProperties()`
at `DeviceVulkan.cpp:472-479`.

**Device extensions are NOT filtered at all.** There is no
`vk::PhysicalDevice::enumerateDeviceExtensionProperties()` call anywhere in the
file -- `kDeviceExtensions` is passed straight through:

```cpp
auto deviceInfo = vk::DeviceCreateInfo()                                  // :610
    .setEnabledExtensionCount((uint32_t)std::size(kDeviceExtensions))     // :613
    .setPpEnabledExtensionNames(kDeviceExtensions)                        // :614
    .setPNext(&vulkan12Features);                                         // :615
m_device = m_physicalDevice.createDevice(deviceInfo);                     // :619
```

### F-5c -- what Task 6 must change (exact edit sites)

1. **Add an availability query.** Insert a
   `m_physicalDevice.enumerateDeviceExtensionProperties()` sweep after the
   physical-device pick (`DeviceVulkan.cpp:559-570`) and before the device
   create block, mirroring the instance-extension shape at `:495-504`.
2. **Make `kDeviceExtensions` a `std::vector<const char*>`** built from the
   constant array at `:42-44` plus the optionally-available diagnostics
   extensions -- the same pattern `instanceExtensions` already uses at `:490`.
3. **Update BOTH consumers.** `deviceInfo` at `:613-614` AND the NVRHI
   `DeviceDesc` at `:643-644`:
   ```cpp
   nvrhiDesc.deviceExtensions    = kDeviceExtensions;         // :643
   nvrhiDesc.numDeviceExtensions = std::size(kDeviceExtensions); // :644
   ```
   Missing `:643-644` would leave NVRHI believing the extension is absent.
4. **Chain the feature struct.** `VK_EXT_device_fault` requires enabling
   `VkPhysicalDeviceFaultFeaturesEXT::deviceFault`. The existing pNext chain is
   `deviceInfo -> vulkan12Features (:606-608) -> vulkan13Features (:602-604)`;
   append the fault features struct to the tail.

### F-5d -- exact extension symbols (vendored Vulkan-Headers)

`ThirdParty/Vulkan-Headers/include/vulkan/vulkan_core.h`:

| Symbol | Line | Value / signature |
|---|---|---|
| `VK_EXT_DEVICE_FAULT_SPEC_VERSION` | 20911 | `2` |
| `VK_EXT_DEVICE_FAULT_EXTENSION_NAME` | 20912 | `"VK_EXT_device_fault"` |
| `VkPhysicalDeviceFaultFeaturesEXT` | 20917-20922 | feature struct (`deviceFault`, `deviceFaultVendorBinary`) |
| `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT` | 988 | `1000341000` |
| `VkDeviceFaultCountsEXT` | 20924-20930 | counts query struct |
| `VkDeviceFaultInfoEXT` | 20932-20939 | info struct |
| `vkGetDeviceFaultInfoEXT` | 20951-20954 | `VkResult (VkDevice, VkDeviceFaultCountsEXT*, VkDeviceFaultInfoEXT*)` (PFN typedef at 20947) |
| `VK_AMD_BUFFER_MARKER_SPEC_VERSION` | 18096 | `1` |
| `VK_AMD_BUFFER_MARKER_EXTENSION_NAME` | 18097 | `"VK_AMD_buffer_marker"` |
| `vkCmdWriteBufferMarkerAMD` | 18103 | `(VkCommandBuffer, VkPipelineStageFlagBits, VkBuffer, VkDeviceSize, uint32_t)` (PFN typedef at 18098) |
| `vkCmdWriteBufferMarker2AMD` | 18112 | `(VkCommandBuffer, VkPipelineStageFlags2, VkBuffer, VkDeviceSize, uint32_t)` (PFN typedef at 18099) |

`vkGetDeviceFaultInfoEXT` is the two-call idiom: call once with
`pFaultInfo == nullptr` to fill counts, allocate, call again.

**Dispatcher note.** Arcane uses the Vulkan-Hpp default dynamic dispatcher
(`VULKAN_HPP_DEFAULT_DISPATCHER.init(...)` at `DeviceVulkan.cpp:465`, `:528`,
`:631`; storage TU `ArcaneClient/src/Arcane/Render/VulkanDispatchStorage.cpp`).
Extension entry points resolve automatically through the `init(m_device)` call at
`:631` -- but only if the extension was enabled at device creation, so the
ordering in F-5c step 3 is load-bearing.

**`VK_AMD_buffer_marker` prefers `MEMORY_PROPERTY_HOST_VISIBLE | HOST_COHERENT`
host memory** for the marker buffer -- the Vulkan analogue of F-1a's system-memory
requirement. There is no `OpenExistingHeapFromAddress` equivalent; a
host-coherent, host-visible, persistently-mapped `VkBuffer` is the recipe.

---

## F-6: `Diagnostics` internals -- report composition and `dumpDir`

### F-6a -- report directory

`ArcaneClient/src/Arcane/Base/Diagnostics.cpp:77-98` -- `ReportDir()`:

```cpp
if (!g_cfg.dumpDir.empty())            // :82
    dir = std::filesystem::path(g_cfg.dumpDir);   // :84
else
{
    const std::string exe = ExecutablePathUtf8();  // :90
    dir = exe.empty() ? std::filesystem::path(".")
                      : std::filesystem::path(exe).parent_path();  // :91-92
    dir /= "diagnostics";                                          // :93
}
std::filesystem::create_directories(dir, ec);      // :96
```

`Config::dumpDir` is declared at
`ArcaneClient/src/Arcane/Base/Diagnostics.hpp:49` (doc comment `:46-48`:
"Empty => `<exe dir>/diagnostics`").

### F-6b -- sibling path minting (the seam a `.arcdiag` sibling extends)

`Diagnostics.cpp:326-344` -- `WriteReportImpl(const char* reason, void* exceptionPointers)`:

```cpp
std::lock_guard reportLock(g_reportMutex);                  // :329
const std::filesystem::path dir   = ReportDir();            // :333
const std::string           stamp = TimeStampForFilename(); // :334
const std::string           base  = g_cfg.appName + "-" + stamp + "-pid" +
                                    std::to_string(GetCurrentProcessId());  // :335-336
const std::filesystem::path dmpPath = dir / (base + ".dmp");   // :338
const std::filesystem::path txtPath = dir / (base + ".txt");   // :339
const bool dumpOk = WriteMiniDump(dmpPath, ep);                // :344
```

**=> `base` (`Diagnostics.cpp:335-336`) is the sibling-path stem.** A third
sibling is exactly `dir / (base + ".arcdiag")`, minted at the same site. Filename
shape: `<appName>-YYYYMMDD-HHMMSS-pid<N>.<ext>`; the stamp comes from
`TimeStampForFilename()` (`Diagnostics.cpp:100-112`, `"%04u%02u%02u-%02u%02u%02u"`
from `GetLocalTime`).

Composition order in the same function: minidump first (deliberate, comment at
`:341-343`), then the text body is accumulated into `std::string out` via the
`append` lambda (`:346-348`): header block `:350-372`
(`reason`/`app`/`pid`/`main thread`/`phase`/`since beat`/`minidump`/`exception`),
then every thread's symbolized stack `:378-412`, then the file write
`:414-418` (`fopen_s` + `fwrite`), then `g_reportCount.fetch_add` `:420` and an
`ARC_ERROR` echo of the whole report into the engine log `:424`.

Minidump flags: `MiniDumpWithThreadInfo | MiniDumpWithHandleData |
MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory`
(`Diagnostics.cpp:314-316`), written by `WriteMiniDump` (`:299-322`).

Public entry: `std::string WriteReport(const char* reason)` at
`Diagnostics.cpp:554-557`, declared `Diagnostics.hpp:84`. Crash trigger calls
`WriteReportImpl("crash (unhandled exception)", ep)` at `:439`; the hang watchdog
calls `WriteReportImpl(msg, nullptr)` at `:481`.

### F-6c -- where hosts set `dumpDir` today: NOWHERE

Repo-wide grep for `dumpDir` over `*.cpp`/`*.hpp` excluding `ThirdParty/` returns
exactly six hits and **no host among them**:

- `Diagnostics.cpp:82`, `:84` (the reader)
- `Diagnostics.hpp:49` (the declaration)
- `ArcaneTests/src/DiagnosticsTest.cpp:67`, `:128`, `:182` (tests, into a temp dir)

Both hosts install with `dumpDir` left empty, so both write to
`<exe dir>/diagnostics`:

- `ArcaneEditor/src/main.cpp:64-66`:
  `Arcane::Diagnostics::Config diag; diag.appName = "ArcaneEditor";
  Arcane::Diagnostics::Install(diag);`
- `ArcaneRuntime/src/main.cpp:42-44`: same three lines with
  `diag.appName = "ArcaneRuntime"`.

**=> `<exe dir>/diagnostics` is where a `.arcdiag` will land unless Task 5/6
changes a host.** If the arc wants `.arcdiag` under a project's `diag://` mount
instead, the change is either a `dumpDir` assignment at those two host sites, or
a separate path in the new writer -- but note the existing `.dmp`/`.txt` siblings
would then split across two directories. Prefer keeping all three siblings
together and mounting `diag://` at the report dir.

Frame-loop liveness (context for hang-vs-GPU-hang disambiguation):
`Arcane::Diagnostics::Heartbeat()` is called first in each host frame --
`ArcaneEditor/src/App/EditorAppFrame.cpp:193` and
`ArcaneRuntime/src/RuntimeApp.cpp:346`; phase labels set at
`EditorAppFrame.cpp:186` ("editor frame loop") and `RuntimeApp.cpp:340`
("runtime frame loop").

---

## F-7: registry scan classification + the incremental-add call

### F-7a -- `ScanContent` and where classification actually lives

`ArcaneClient/src/Arcane/Project/AssetRegistry.cpp:131` --
`std::size_t AssetRegistry::ScanContent(const std::filesystem::path& contentDir,
std::string_view scheme, ScanProgressFn onProgress)` (declared
`AssetRegistry.hpp:66`).

`ScanContent` does NOT classify. It clears `m_byGuid` and `m_scanDiagnostics`
(`:134-138`), delegates to `AddContent` when there is no progress callback
(`:143-144`), otherwise does a counting pass (`:155-158`) plus a walk that calls
`AddFile` per file (`:162-172`), and publishes the `"assets"` diagnostic group
once at `:180`. `AddContent` (`:185-217`) is the same walk without progress,
publishing at `:214`.

**Classification lives in `AssetRegistry::AddFile`**, declared
`AssetRegistry.hpp` / defined `AssetRegistry.cpp:219-221`:

```cpp
const std::string ext = LowerExt(file);          // :223
if (ext == ".meta") return std::nullopt;         // :226-227  (sidecar, not an asset)

Guid id; bool idWriteFailed = false;                                   // :240-241
if (ext == ".json" || ext == ".arcmat" || ext == ".arcscene" || ext == ".arcsprite")
    id = ResolveNativeId(file, &idWriteFailed);                        // :242-243
else if (IsImportedBinary(ext))
    id = ResolveSidecarId(file, &idWriteFailed);                       // :244-245
else
    return std::nullopt;                                               // :246-247
if (!id.IsValid()) return std::nullopt;                                // :249-250
```

- **Native (embedded GUID) extensions -- `AssetRegistry.cpp:242`:**
  `.json`, `.arcmat`, `.arcscene`, `.arcsprite`.
- **Imported-binary (sidecar GUID) extensions -- `IsImportedBinary`,
  `AssetRegistry.cpp:31-42`:** `.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp`, `.hdr`,
  `.wav`, `.ogg`, `.mp3`, `.flac`, `.ttf`, `.otf`.
- Anything else is not tracked.

**Embedded-GUID read: `ResolveNativeId`, `AssetRegistry.cpp:61-86.`** Parses the
file as JSON (`:65`), reads the top-level `"id"` field via `ReadGuidField(doc,
"id")` (`:73-74`); if absent it MINTS one (`Guid::Generate()`, `:76`), writes it
back into the file (`:77-79`), and sets `*writeFailed` if the write fails
(`:82-83`). Sidecar equivalent is `ResolveSidecarId` (`AssetRegistry.cpp:95`, doc comment
`:88-94`; `hero.png` -> `hero.png.meta`, extension APPENDED not replaced).

**=> A new `.arcdiag` extension must be added to the native list at
`AssetRegistry.cpp:242` (it carries an embedded top-level `"id"`, exactly like
`.arcmat`/`.arcsprite`).** The comment block at `:229-239` is where the reason
gets recorded, alongside the existing `.arcscene` and `.arcsprite` rationales.

### F-7b -- the exact incremental-registration call for one new asset

`EditorApp::CreateMaterialAt(std::filesystem::path path)` --
`ArcaneEditor/src/App/EditorAppProject.cpp:309`. It saves the asset
(`SaveMaterialAsset`, `:343`) and then registers exactly one file:

```cpp
m_runtime->RegisterCreatedAsset(path);   // EditorAppProject.cpp:350
m_documents.OpenPath(path);              // :351
```

**=> `Runtime::RegisterCreatedAsset(const std::filesystem::path&)` is THE
single-asset registration call.** Full chain, all verified:

1. `EditorAppProject.cpp:350` -> `m_runtime->RegisterCreatedAsset(path)`
   (declared `ArcaneClient/src/Arcane/Base/Runtime.hpp:130`).
2. `ArcaneClient/src/Arcane/Base/Runtime.cpp:464-473` -- warns + returns
   `std::nullopt` when no project is open (`:466-471`), else
   `return m_impl->project->RegisterAsset(file);` (`:472`).
3. `ArcaneClient/src/Arcane/Project/Project.cpp:400-429` --
   `Project::RegisterAsset` canonicalizes (`:403-404`), builds the candidate
   content-root list `game://` first then each active plugin root (`:407-412`),
   finds the root containing the file (`:414-421`), and calls
   `return m_registry.AddFile(target, rootDir, root.scheme);` (`:422`).
   Outside every root it warns and returns `std::nullopt` (`:425-428`).
4. `AssetRegistry::AddFile` (`AssetRegistry.cpp:219`) -- the classifier in F-7a.

Returns `std::optional<Guid>`. Other callers of the same seam, for reference:
`EditorAppProject.cpp:231`, `:304`, `EditorAppScene.cpp:278`,
`Documents/ShaderEditorDocument.cpp:1549`.

Caveat worth carrying into Task 7: `AddFile` appends to `m_scanDiagnostics` but
never publishes (comment `AssetRegistry.cpp:252-260`), so a standalone
registration's diagnostics surface only on the next `ScanContent`/`AddContent`.

---

## F-8: the frame's pass seams (the list Task 7 instruments)

### F-8a -- ArcaneEditor: `EditorApp::MainLoop`, `EditorAppFrame.cpp:178-236`

Twenty numbered phases; the body is a flat call list at `:195-234`. GPU-bearing
phases only (each is a separate function; each is a scope-marker candidate):

| # | Function | Definition | GPU work |
|---|---|---|---|
| 10 | `RenderSceneToViewport()` | `EditorAppFrame.cpp:1041`, called `:220` | Scene -> `m_viewportTargets.canvas->Draw(...)` (`:1049`), which is open/clear/batcher/tonemap/close/execute -- see F-8c. Gizmo + camera-rect lines recorded inside the same lambda (`:1080-1125`). |
| 11 | `CompositeGameUi()` | `:1132`, called `:221` | Play-mode only. `m_gpu->Cmd()->open()` `:1165`, `m_gameImgui->Render(...)` `:1166`, `close()` `:1167`, `executeCommandList` `:1168`. |
| 12 | `RenderSelectionOutline()` | `:1175`, called `:222` | Edit-mode only, mutually exclusive with 11. `m_viewportTargets.pick->RenderIdPass(...)` `:1185`; then `Cmd()->open()` `:1204`, `m_viewportTargets.outline->Render(...)` `:1205-1206`, `close()` `:1207`, `executeCommandList` `:1208`. |
| 19 | `PresentFrame()` | `:1856`, called `:230` | `m_gpu->Swap().BeginFrame()` `:1858`; `Cmd()->open()` `:1861`; `clearTextureFloat(backbuffer, ...)` `:1864-1865`; `m_gpu->Imgui().Render(m_gpu->Cmd(), fb)` `:1867`; `close()` `:1868`; `executeCommandList` `:1869`; `m_gpu->Swap().Present()` `:1870`. |

Non-GPU phases for completeness (still useful as CPU-side phase labels):
`PumpFrameEvents` `:240` (called `:195`), `ConsumeDeferredSceneAction` `:306`,
`ConsumeSceneDialogResults` `:322`, `ConsumeProjectDialogResult` `:367`,
`ConsumeMaterialDialogResults` `:386`, `PollModuleBuild` (called `:213`),
`FrameInput` `:407` (`:216`), `AdvanceSim` `:907` (`:217`),
`ApplyPendingViewportResize` `:921` (`:218`), `RefreshSceneResolution` `:947`
(`:219`), `PumpEditorDocuments` `:1221` (`:223`), `DrawEditorUi` `:1232` (`:224`),
`DrawModals` `:1593` (`:225`), `DrawViewportPanelPhase` `:1714` (`:226`),
`SyncCenterTabFocus` `:1750` (`:227`), `HandleViewportPick` `:1795` (`:228`),
`DrawSelectionPanels` `:1834` (`:229`), `EndFrame` `:1875` (`:234`).

Note: `RenderSceneToViewport`, `CompositeGameUi`, and `RenderSelectionOutline`
each execute their own command list; `PresentFrame` executes a fourth. That is
**four submits per editor frame**, so a marker buffer must be readable across all
four -- one buffer with per-scope slots, not one per list.

### F-8b -- ArcaneRuntime: `RuntimeApp.cpp` frame loop (`while (running)` at `:342`)

Single command list per frame -- one `open()` at `:451`, one `close()` at `:561`,
one `executeCommandList` at `:564`, `Present()` at `:565`. The pass seams inside
that one recording, in order:

| Seam | Lines | What |
|---|---|---|
| ImGui host UI build | `:398-406` | `m_gpu->Imgui().BeginFrame()` `:398` (CPU-side) |
| Plugin ImGui build | `:410` | `m_plugin->DrawUIAll()` (CPU-side) |
| Backbuffer acquire | `:412-419` | `m_gpu->Swap().BeginFrame()`; null => `ImGui::EndFrame(); continue;` |
| Shader hot-reload poll | `:422-429` | `m_gpu->Shaders().Poll()` |
| Scene resolve | `:440-449` | `m_resolver->Refresh(frame)`; must precede batcher Begin (comment `:431-439`) |
| **Command list open + canvas clear** | `:451-453` | `Cmd()->open()`; `clearTextureFloat(m_gpu->Cnv().Texture(), ...)` |
| **Batcher pass BEGIN** | `:455-461` | `m_gpu->Batch().Begin(Cmd(), Cnv().Framebuffer(), W, H)`; `SetGlobals` |
| Scene camera push | `:474-509` | `ActiveSceneCamera` -> `m_runtime->SetCamera` (CPU-side) |
| **Scene submission** | `:511-516` | `m_runtime->SetRenderContext(&m_gpu->Batch())` `:513`; `m_runtime->Loop().SubmitRender()` `:514` |
| **Batcher pass END** | `:518-522` | `m_gpu->Batch().End()` `:520` |
| **Post chain (optional)** | `:525-548` | `postChain->Render(Cmd(), post->Framebuffer(), ...)` `:542-546` |
| **Tonemap** | `:547` or `:551` | `m_gpu->Tone().Run(Cmd(), <post or canvas>->Texture(), fb)` |
| **ImGui pass** | `:555-559` | `m_gpu->Imgui().Render(Cmd(), fb)` `:557` |
| **Close + submit + present** | `:561-567` | `Cmd()->close()` `:561`; `executeCommandList` `:564`; `Swap().Present()` `:565` |
| Plugin hot-reload poll | `:570-574` | `m_plugin->Poll()` |

`FramePerf` already brackets several of these with `m_perf.Add(m_perf.accX, t0,
Now())` -- `accSim` `:395`, `accRec` `:515`, `accEnd` `:521`, `accTone` `:553`,
`accImgui` `:558`, `accPresent` `:566`, `accPoll` `:573`
(`ArcaneClient/src/Arcane/Host/FramePerf.hpp`). Task 7's GPU scopes should use
the SAME names so CPU and GPU timelines line up on one vocabulary.

### F-8c -- the shared canvas pass (both hosts, one implementation)

`ArcaneClient/src/Arcane/Render/OffscreenCanvas.cpp`:

- `void Draw(FunctionRef<void(Batcher2D&)> fn, glm::vec4 clear)` -- `:54-79`.
  `m_commandList->open()` `:64`; `clearTextureFloat(m_canvas->Texture(), ...)`
  `:65-67`; `m_batcher->Begin(m_commandList, m_canvas->Framebuffer(), m_width,
  m_height)` `:69-70`; caller lambda `fn(*m_batcher)` `:72`;
  `m_batcher->End()` `:73`; `RunPostAndTonemap()` `:75`;
  `m_commandList->close()` `:77`; `m_device->executeCommandList(m_commandList)`
  `:78`.
- `void DrawPass(FunctionRef<void(nvrhi::ICommandList*, nvrhi::IFramebuffer*)> fn,
  glm::vec4 clear)` -- `:81-103`. Same shape, caller records raw against the
  linear canvas framebuffer (`:97`), tonemap at `:99`.

The header comment at `:60-63` states the contract Task 7 relies on: "Mirror
ArcaneRuntime's main render loop: open -> clear canvas -> batcher Begin/draw/End
-> tonemap Run -> close -> execute. NVRHI manages all framebuffer transitions;
no manual barriers here."

**=> Instrumenting `OffscreenCanvas::Draw`/`DrawPass` covers the editor's
viewport, every document preview, and any other offscreen consumer in one place.**
Note each `OffscreenCanvas` owns its OWN command list (`m_commandList`, created
`:43`), distinct from `GpuContext::Cmd()`.

### F-8d -- the host command-list seam

`Arcane::GpuContext` (`ArcaneClient/src/Arcane/Host/GpuContext.hpp:38`) is where
both hosts get their shared objects: `Device()` `:64`, `Swap()` `:65`,
`Shaders()` `:66`, `Cnv()` `:67`, `Batch()` `:68`, `Tone()` `:69`, `Imgui()`
`:70`, `Cmd()` `:73` (returns `nvrhi::ICommandList*`),
`FramebufferFor(nvrhi::ITexture*)` `:53`. A marker-buffer owner most naturally
lives beside these -- but heed the teardown contract in the file header comment
(`:11-14`): "MEMBER DECLARATION ORDER IS THE TEARDOWN CONTRACT ... commandList +
framebuffers LAST (release their NVRHI handles before device). Do NOT reorder."
A marker buffer holding an `ID3D12Heap`/`VkBuffer` must be declared so it
releases before the device.

Also GPU-bearing, outside the frame loop: `BootPresenter`
(`ArcaneClient/src/Arcane/Host/BootPresenter.cpp` -- `Imgui().BeginFrame()` `:32`,
`Swap().BeginFrame()` `:74`, `executeCommandList` `:98`, `Swap().Present()`
`:99`). Boot-time device removal is real; Task 7's markers should be armed before
this runs, or the boot path explicitly excluded and said so.

---

## Summary of consequences for Tasks 5-7

1. **Task 5 must bind against the vendored `ThirdParty/DirectX-Headers/include/directx/d3d12.h`**, not the Windows SDK header -- `#include <d3d12.h>` resolves there (empirically verified). Both carry identical DRED symbols, so this is a citation/robustness point, not an API difference.
2. **The lightweight DRED tier EXISTS** (`UseMarkersOnlyAutoBreadcrumbs`, Settings2). The spec's Dist row does not need the anticipated breadcrumbs-only amendment -- unless F-2d's runtime QI check fails at desk, which the tier ladder must detect and log.
3. **The one cross-backend device-removed hook is `NvrhiMessageCallback::message`** (`NvrhiMessageCallback.hpp:26`, error branch `:36-39`) -- NVRHI reports `"Device Removed!"` from both `d3d12-device.cpp:630` and `vulkan-queue.cpp:200`. `DeviceD3D12.cpp:280` is the only first-party site and Vulkan has none.
4. **Marker buffers go in `VirtualAlloc` + `OpenExistingHeapFromAddress` memory**, gated on `D3D12_FEATURE_EXISTING_HEAPS`; begin=`MARKER_IN`, end=`MARKER_OUT`; destination resource in `D3D12_RESOURCE_STATE_COPY_DEST`; command list must be QI'd up to `ID3D12GraphicsCommandList2`.
5. **`.arcdiag` is a native embedded-GUID asset** -- add it at `AssetRegistry.cpp:242`; single-asset registration is `Runtime::RegisterCreatedAsset` (`Runtime.cpp:464`).
6. **Reports land in `<exe dir>/diagnostics`** because no host sets `dumpDir`; the sibling stem is `Diagnostics.cpp:335-336`.
7. **Editor = 4 submits/frame, Runtime = 1** -- the marker buffer must be shared across submits, not per-command-list.
