// See NriCommon.hpp for the include-order rationale (nri::Message::ERROR vs
// wingdi.h's ERROR macro) -- the NRI headers MUST stay first in this file.
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

#include "NriCommon.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/NvrhiMessageCallback.hpp>

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

        // Same gate, new producer: NvrhiMessageCallback::message() both logs
        // at ARC_ERROR (with the "[nvrhi]" tag it always uses, regardless of
        // producer -- see DeviceVulkan.cpp's VkDebugCallback for the existing
        // precedent of a non-nvrhi producer routing through this exact call)
        // and increments the SAME atomic Device.cpp's RenderErrorCount()
        // reads. Routing NRI errors through it here, rather than adding a
        // second counter, keeps the 0/0 gate a single source of truth.
        void RouteNriError(const char* text)
        {
            NvrhiMessageCallback::Instance().message(nvrhi::MessageSeverity::Error, text);
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
    }

    bool NriCheckImpl(nri::Result result, const char* expr, const char* file, int line)
    {
        if (result == nri::Result::SUCCESS)
            return true;

        char buffer[1024];
        std::snprintf(buffer, sizeof(buffer), "[nri] %s failed: %s (%s:%d)",
                      expr ? expr : "", NriResultName(result), file ? file : "", line);
        RouteNriError(buffer);
        return false;
    }

    nri::CallbackInterface MakeNriCallbacks() noexcept
    {
        nri::CallbackInterface callbacks{};
        callbacks.MessageCallback = &NriMessageCallback;
        callbacks.AbortExecution  = nullptr; // let the engine decide whether to keep running past an ERROR
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
        // property) -- mirror the same compile-time default Render/Device.hpp
        // already uses for RenderDeviceDesc::enableValidation.
#if defined(ARCANE_DEBUG)
        constexpr const char* kValidation = "on";
#else
        constexpr const char* kValidation = "off";
#endif

        ARC_INFO("[nri] v{} backend={} validation={}",
                 desc.nriVersion, NriBackendName(desc.graphicsAPI), kValidation);
    }
}
