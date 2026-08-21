// See NriCommon.hpp for the include-order rationale (nri::Message::ERROR vs
// wingdi.h's ERROR macro) -- the NRI headers MUST stay first in this file.
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

#include "NriCommon.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>

// Arcane/Base/Log.hpp -> spdlog pulls in <windows.h>, whose wingdi.h
// unconditionally #defines ERROR (a GDI region-type constant, value 0).
// Preprocessor substitution is purely textual: it does not respect C++
// scope, so it corrupts every LATER bare "ERROR" token in this translation
// unit too, including the qualified nri::Message::ERROR case label below
// (observed: MSVC C2589/C2062, "case nri::Message::0"). Undefine it right
// after the last header that could define it, before any nri::Message::ERROR
// use in this file.
#undef ERROR

#include <cstdio>

namespace Arcane
{
    namespace
    {
        const char* NriResultName(nri::Result result)
        {
            switch (result)
            {
            case nri::Result::DEVICE_LOST:      return "DEVICE_LOST";
            case nri::Result::OUT_OF_DATE:      return "OUT_OF_DATE";
            case nri::Result::INVALID_SDK:      return "INVALID_SDK";
            case nri::Result::SUCCESS:          return "SUCCESS";
            case nri::Result::FAILURE:          return "FAILURE";
            case nri::Result::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
            case nri::Result::OUT_OF_MEMORY:    return "OUT_OF_MEMORY";
            case nri::Result::UNSUPPORTED:      return "UNSUPPORTED";
            }
            return "UNKNOWN";
        }

        const char* NriBackendName(nri::GraphicsAPI api)
        {
            switch (api)
            {
            case nri::GraphicsAPI::NONE:  return "NONE";
            case nri::GraphicsAPI::D3D11: return "D3D11";
            case nri::GraphicsAPI::D3D12: return "D3D12";
            case nri::GraphicsAPI::VK:    return "VK";
            case nri::GraphicsAPI::WGPU:  return "WGPU";
            }
            return "Unknown";
        }

        // Same gate, new producer: RenderErrorLatch::NoteNriError() both
        // logs at ARC_ERROR (with the "[nri]" tag it always uses, regardless
        // of producer -- see DeviceCreationVulkan.cpp's VkDebugCallback for the
        // other producer, the Vulkan validation layer, routing through this
        // exact call) and increments the SAME atomic RenderErrorCount() reads.
        // Routing NRI errors through it here, rather than adding a second
        // counter, keeps the 0/0 gate a single source of truth.
        void RouteNriError(const char* text)
        {
            RenderErrorLatch::Instance().NoteNriError(text);
        }

        // NRI's own MessageCallback signature (Extensions/NRIDeviceCreation.h):
        // void (NRI_CALL *)(nri::Message, const char* file, uint32_t line,
        // const char* message, void* userArg). NRI_CALL is __stdcall on
        // Windows, so the calling convention here must match exactly.
        void NRI_CALL NriMessageCallback(nri::Message messageType, const char* file,
                                          uint32_t line, const char* message, void* /*userArg*/)
        {
            char buffer[1024];
            std::snprintf(buffer, sizeof(buffer), "[nri] %s:%u: %s",
                          file ? file : "", line, message ? message : "");

            switch (messageType)
            {
            case nri::Message::INFO:
                ARC_INFO("{}", buffer);
                break;
            case nri::Message::WARNING:
                ARC_WARN("{}", buffer);
                break;
            case nri::Message::ERROR:
                RouteNriError(buffer);
                break;
            default:
                // nri::Message::MAX_NUM is a sentinel NRI itself never passes
                // to a callback; this default exists only so an unexpected
                // future value doesn't silently vanish.
                ARC_WARN("{}", buffer);
                break;
            }
        }

        // NRI invokes this after every ReportMessage whose Result is
        // non-SUCCESS (Shared/SharedExternal.hpp: "if
        // (m_CallbackInterface.AbortExecution && (int8_t)result > 0)"), and
        // NRI's OWN default for the slot is DebugBreak()
        // (Creation/Creation.cpp:68).
        //
        // This function exists because leaving the field null does NOT opt
        // out: Creation.cpp:140-141 fills a null AbortExecution with that
        // DebugBreak default, silently inverting the intent this file used to
        // state as `AbortExecution = nullptr`. Desk checkpoint D3b caught the
        // consequence -- a recoverable dx12 CreateBufferView failure inside
        // the --crash-gpu injector became an unhandled STATUS_BREAKPOINT
        // (0x80000003) that killed the process before the host's own
        // device-removed reporting and exit-1 path could run, while the vulkan
        // twin exited cleanly. A no-op is the only real opt-out.
        //
        // The MESSAGE channel is untouched: NriMessageCallback above still
        // logs every ERROR and still routes it into RenderErrorCount, so the
        // error latch and the zero-errors gate keep their teeth, and
        // ARC_NRI_CHECK still sees the typed failing Result at the call site.
        // Only the process-breaking half goes -- matching how the D3D12 debug
        // layer is armed for the same class of signal
        // (Render/DeviceCreationD3D12.cpp arms the D3D12 InfoQueue with
        // SetBreakOnSeverity(..., FALSE) on all three severities and lets the
        // host do the reporting).
        void NRI_CALL NriAbortExecution(void* /*userArg*/)
        {
            // Deliberately empty. See above.
        }
    }

    bool NriCheckImpl(nri::Result result, const char* expr, const char* file, int line)
    {
        if (result == nri::Result::SUCCESS)
            return true;

        char buffer[1024];
        std::snprintf(buffer, sizeof(buffer), "[nri] %s failed: %s (%s:%d)",
                      expr ? expr : "", NriResultName(result), file ? file : "", line);

        // TYPED device-loss observation, made here rather than at each call
        // site because here is where the typed result actually is. NRIDescs.h
        // documents DEVICE_LOST as returnable by
        // "QueueSubmit*", "*WaitIdle", "AcquireNextTexture", "QueuePresent"
        // and "WaitForPresent" -- every one of which this tree already funnels
        // through ARC_NRI_CHECK (NriSwapChain's acquire/present/pacing-submit,
        // the graph executor's submit, NriDevice's teardown wait). So one
        // branch covers the whole NRI path at once. A message-substring-only
        // observation could never match it -- RouteNriError's text says
        // "DEVICE_LOST" while NotifyIfDeviceRemoved looks for "Device
        // Removed" -- so this is what reaches the real ObserveDeviceRemoved
        // chain that writes the .arcdiag/.gpudump pair and latches the hosts'
        // shutdown.
        //
        // Safe to fire the hook from here: every caller is our own code on
        // its own thread, never a driver/validation callback re-entering us
        // (which is the hazard NoteError exists to avoid).
        if (result == nri::Result::DEVICE_LOST)
        {
            RenderErrorLatch::Instance().NoteDeviceLost("nri", buffer);
            return false;
        }

        RouteNriError(buffer);
        return false;
    }

    nri::CallbackInterface MakeNriCallbacks() noexcept
    {
        nri::CallbackInterface callbacks{};
        callbacks.MessageCallback = &NriMessageCallback;
        // NOT nullptr: NRI fills a null slot with its DebugBreak default
        // (Creation.cpp:140-141). The explicit no-op is what actually lets the
        // engine decide whether to keep running past an ERROR.
        callbacks.AbortExecution  = &NriAbortExecution;
        callbacks.userArg         = nullptr;
        return callbacks;
    }

    void LogNriIdentity(nri::Device& device)
    {
        nri::CoreInterface core{};
        if (!ARC_NRI_CHECK(nriGetInterface(device, NRI_INTERFACE(nri::CoreInterface), &core)))
            return;

        const nri::DeviceDesc& desc = core.GetDeviceDesc(device);

        // NRI's DeviceDesc carries no validation-state field (validation is a
        // wrapping choice made at creation time, not a queryable device
        // property) -- mirror the same compile-time default
        // Render/RenderDeviceDesc.hpp already uses for
        // RenderDeviceDesc::enableValidation.
#if defined(ARCANE_DEBUG)
        constexpr const char* kValidation = "on";
#else
        constexpr const char* kValidation = "off";
#endif

        ARC_INFO("[nri] v{} backend={} validation={}",
                 desc.nriVersion, NriBackendName(desc.graphicsAPI), kValidation);
    }
}
