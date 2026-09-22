// Crash window plan 1 (Task 3): PortableStack implementation. See the
// header for the "why" -- CaptureStackFromContext is deliberately free of
// C++ locals that need unwinding (the CONTEXT copy and the std::span
// parameter are both trivially destructible), because __try/__except may
// not live in a function that has such locals (MSVC C2712).

#include <Arcane/Base/PortableStack.hpp>

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Arcane::Diagnostics
{
#if defined(_WIN32)
    std::size_t CaptureStackFromContext(const void* nativeContext, std::span<StackFrame> out) noexcept
    {
        if (!nativeContext || out.empty())
            return 0;

        CONTEXT ctx = *static_cast<const CONTEXT*>(nativeContext);   // a COPY: unwinding mutates it
        std::size_t n = 0;
        __try
        {
            while (n < out.size() && ctx.Rip != 0)
            {
                out[n].address = ctx.Rip;
                out[n].module  = ModuleTable::Find(ctx.Rip);
                ++n;

                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (fn)
                {
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
                }
                else
                {
                    // Leaf function: the return address is at RSP.
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { /* stop where the walk faulted; n frames are valid */ }
        return n;
    }

    std::size_t CaptureCurrentStack(std::span<StackFrame> out) noexcept
    {
        CONTEXT ctx{};
        RtlCaptureContext(&ctx);
        return CaptureStackFromContext(&ctx, out);
    }
#else
    std::size_t CaptureStackFromContext(const void*, std::span<StackFrame>) noexcept { return 0; }
    std::size_t CaptureCurrentStack(std::span<StackFrame>) noexcept { return 0; }
#endif

    std::string_view FormatStackFrame(std::size_t index, const StackFrame& f, std::span<char> buffer) noexcept
    {
        if (buffer.empty())
            return {};

        int written;
        if (f.module != nullptr)
        {
            const std::uint64_t offset = f.address - f.module->base;
            written = std::snprintf(buffer.data(), buffer.size(), "  %02zu  %s + 0x%llx (base 0x%016llx)",
                                     index, f.module->name,
                                     static_cast<unsigned long long>(offset),
                                     static_cast<unsigned long long>(f.module->base));
        }
        else
        {
            written = std::snprintf(buffer.data(), buffer.size(), "  %02zu  0x%016llx <unloaded or unknown module>",
                                     index, static_cast<unsigned long long>(f.address));
        }

        if (written < 0)
        {
            buffer[0] = '\0';
            return std::string_view(buffer.data(), 0);
        }

        const std::size_t len = static_cast<std::size_t>(written) < buffer.size()
                                     ? static_cast<std::size_t>(written)
                                     : buffer.size() - 1;
        return std::string_view(buffer.data(), len);
    }
}
