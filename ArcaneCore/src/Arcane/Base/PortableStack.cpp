// Crash window plan 1 (Task 3): PortableStack implementation. See the
// header for the "why" -- CaptureStackFromContext is deliberately free of
// C++ locals that need unwinding (the CONTEXT copy and the std::span
// parameter are both trivially destructible), because __try/__except may
// not live in a function that has such locals (MSVC C2712).

#include <Arcane/Base/PortableStack.hpp>

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unwind.h>   // _Unwind_Backtrace: the .eh_frame walk (GCC and Clang alike)
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
    // Linux port (2026-10-05). Walking a FOREIGN context (a signal handler's
    // ucontext_t, the crash path's input) is the Diagnostics crash-path port's
    // job and stays 0 until it lands. The calling thread's own stack is
    // walkable today: _Unwind_Backtrace drives the same .eh_frame unwind
    // tables C++ exceptions use -- the ELF reading of RtlVirtualUnwind over
    // the PE's unwind metadata. No DbgHelp-style symbol load and no heap; it
    // may take the loader's lock to find .eh_frame (dl_iterate_phdr), which
    // is fine off the crash path, the only place this is called from today.
    std::size_t CaptureStackFromContext(const void*, std::span<StackFrame>) noexcept { return 0; }

    namespace
    {
        struct UnwindState
        {
            std::span<StackFrame> out;
            std::size_t           n    = 0;
            std::size_t           skip = 1;   // CaptureCurrentStack's own frame
        };

        _Unwind_Reason_Code CollectFrame(_Unwind_Context* context, void* user)
        {
            auto& state = *static_cast<UnwindState*>(user);
            const std::uint64_t ip = static_cast<std::uint64_t>(_Unwind_GetIP(context));
            if (ip == 0)
                return _URC_END_OF_STACK;
            if (state.skip > 0)
            {
                --state.skip;
                return _URC_NO_REASON;
            }
            if (state.n >= state.out.size())
                return _URC_END_OF_STACK;
            state.out[state.n].address = ip;
            state.out[state.n].module  = ModuleTable::Find(ip);
            ++state.n;
            return _URC_NO_REASON;
        }
    }

    std::size_t CaptureCurrentStack(std::span<StackFrame> out) noexcept
    {
        if (out.empty())
            return 0;
        UnwindState state{ out };
        _Unwind_Backtrace(&CollectFrame, &state);
        return state.n;
    }
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
