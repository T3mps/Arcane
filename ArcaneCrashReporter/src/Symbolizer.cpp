#include "Symbolizer.hpp"
#include "Win32Text.hpp"

#include <Arcane/Base/Engine.hpp>

#include <dbgeng.h>
#include <DbgHelp.h>    // the SYMOPT_* constants; UE includes it after dbgeng.h the same way (WindowsPlatformStackWalkExt.cpp:16-17)
#include <wrl/client.h>

#include <cstddef>
#include <cstdio>
#include <utility>

namespace Arcane::Reporter
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        std::string Hex(HRESULT hr)
        {
            char b[16];
            std::snprintf(b, sizeof(b), "0x%08lx", static_cast<unsigned long>(hr));
            return b;
        }

        // R76: dbgeng's string getters return S_FALSE for "your buffer was too
        // small -- here is a TRUNCATED answer and the size I needed". SUCCEEDED
        // admits that, and a truncated answer is not a smaller truth: a clipped
        // "module!function" can lose the '!' and be read as a bare module, and
        // a clipped module PATH is pushed onto the SYMBOL PATH below, where it
        // is simply a wrong directory. Exact success only; anything else means
        // "no answer", and module+offset is the honest fallback.
        [[nodiscard]] bool Exact(HRESULT hr) { return hr == S_OK; }

        // EndSession on EVERY exit from the point the dump opened, including
        // the failure paths. The ComPtrs below would release the client on
        // their own, but "release the last reference and hope" is not the same
        // as detaching the session -- dbgeng keeps the dump file mapped until
        // the session ends, and a reporter that leaves it mapped would keep a
        // lock on the one artifact a human is about to open in windbg.
        struct SessionEnder
        {
            IDebugClient5* client = nullptr;
            ~SessionEnder() { if (client) client->EndSession(DEBUG_END_ACTIVE_DETACH); }

            SessionEnder()                               = default;
            SessionEnder(const SessionEnder&)            = delete;
            SessionEnder& operator=(const SessionEnder&) = delete;
        };

        // UE's rule (WindowsPlatformStackWalkExt.cpp:183-243): the symbol path
        // is the directory of EVERY module the dump names, plus
        // _NT_SYMBOL_PATH, and the image path is the same set. For our layout
        // that puts the host's directory (ArcaneCore.dll/ArcaneClient.dll
        // beside the exe) and the project's Binaries/ (the game module, whose
        // PDB arcbuild leaves beside it) on the path without anyone naming
        // them. The reporter's own directory joins too: it is staged beside
        // the host (spec §12 item 2).
        std::wstring ModuleDirectories(IDebugSymbols3* symbols)
        {
            std::wstring path;
            ULONG loaded = 0, unloaded = 0;
            if (FAILED(symbols->GetNumberModules(&loaded, &unloaded))) return path;
            for (ULONG i = 0; i < loaded; ++i)
            {
                ULONG64 base = 0;
                if (FAILED(symbols->GetModuleByIndex(i, &base))) continue;
                wchar_t image[1024];
                ULONG   size = 0;
                // R76: EXACT success only. A truncated image path here becomes
                // a wrong DIRECTORY on the symbol path -- the one place a
                // "nearly right" answer is worse than none.
                if (!Exact(symbols->GetModuleNameStringWide(DEBUG_MODNAME_IMAGE, i, base, image, 1024, &size)) || size <= 1) continue;
                std::wstring dir(image);
                const std::size_t slash = dir.find_last_of(L"/\\");
                if (slash == std::wstring::npos) continue;
                dir.resize(slash);
                if (path.find(dir + L";") == std::wstring::npos) { path += dir; path += L";"; }
            }
            return path;
        }

        std::wstring DefaultSymbolPath(IDebugSymbols3* symbols)
        {
            std::wstring path = ModuleDirectories(symbols);
            std::wstring self = ToWide(Arcane::ExecutablePathUtf8());
            const std::size_t slash = self.find_last_of(L"/\\");
            if (slash != std::wstring::npos) { self.resize(slash); path += self; path += L";"; }
            // R75. GetEnvironmentVariableW does NOT write the buffer when the
            // value does not fit: it returns the REQUIRED size (including the
            // null) and leaves the buffer untouched. The old `> 0` test passed
            // in exactly that case and then appended an UNINITIALISED
            // wchar_t[4096] until it happened to meet a null word -- undefined
            // behaviour, and a garbage symbol path in practice.
            //
            // This is the DEFAULT path, the one every real hand-off takes, in
            // the process whose whole job is to work when things have already
            // gone wrong. A desk with a short or unset _NT_SYMBOL_PATH can
            // never reproduce it, which is what made it a failure-path defect
            // rather than a happy-path one.
            //
            // Sized with a null buffer first, so a long value is USED rather
            // than dropped: the developer who set a 5 KB symbol path meant it.
            //
            // R113: NOT in Dist. UE's reason (WindowsPlatformStackWalkExt.cpp
            // :22-28): "we don't want shipping crash reporter to try to
            // access build servers" -- _NT_SYMBOL_PATH is exactly that, an
            // env var a build machine or a developer's desk sets to point at
            // one. The explicit --symbol-path seam (opt.symbolPath, checked
            // by the caller above) and the module-directory search
            // (ModuleDirectories, just above) are unaffected in every
            // configuration -- only the ambient env var is shut off here.
#if !defined(ARCANE_DIST)
            const DWORD needed = GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", nullptr, 0);
            if (needed > 1)
            {
                std::wstring env(needed, L'\0');
                const DWORD  wrote = GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", env.data(), needed);
                // `wrote` EXCLUDES the null on success and must be < needed;
                // anything else means the value changed under us between the
                // two calls, and a half-read path is not worth using.
                if (wrote > 0 && wrote < needed)
                {
                    env.resize(wrote);
                    path += env;
                }
            }
#endif
            return path;
        }

        // One frame: dbgeng's "module!function" name + displacement, line info
        // when the PDB has it, else the IMAGE file name + offset from its base.
        SymFrame Resolve(IDebugSymbols3* symbols, ULONG64 address)
        {
            SymFrame f;
            f.address = address;

            wchar_t name[1024];
            ULONG   size = 0;
            ULONG64 disp = 0;
            if (Exact(symbols->GetNameByOffsetWide(address, name, 1024, &size, &disp)) && size > 1)
            {
                const std::string full = ToUtf8(name);
                const std::size_t bang = full.find('!');
                // No '!' means dbgeng had no SYMBOL, only a module -- it
                // spells that "module+0x..." itself. Fall through to the image
                // path below so the symbol-less form always names the IMAGE
                // FILE (death-fixture.exe), one spelling, never two.
                if (bang != std::string::npos)
                {
                    f.module       = full.substr(0, bang);
                    f.function     = full.substr(bang + 1);
                    f.displacement = disp;

                    wchar_t file[1024];
                    ULONG   line     = 0;
                    ULONG   fileSize = 0;
                    ULONG64 lineDisp = 0;
                    if (Exact(symbols->GetLineByOffsetWide(address, &line, file, 1024, &fileSize, &lineDisp)) && fileSize > 1)
                    {
                        f.file = ToUtf8(file);
                        f.line = line;
                    }
                    return f;
                }
            }

            ULONG   index = 0;
            ULONG64 base  = 0;
            if (SUCCEEDED(symbols->GetModuleByOffset(address, 0, &index, &base)))
            {
                wchar_t image[1024];
                ULONG   imageSize = 0;
                if (Exact(symbols->GetModuleNameStringWide(DEBUG_MODNAME_IMAGE, index, base, image, 1024, &imageSize)) && imageSize > 1)
                {
                    const std::string full  = ToUtf8(image);
                    const std::size_t slash = full.find_last_of("/\\");
                    f.module = slash == std::string::npos ? full : full.substr(slash + 1);
                }
                else
                {
                    f.module = "<module>";
                }
                f.displacement = address - base;
                return f;
            }

            f.module       = "<unknown>";
            f.displacement = address;
            return f;
        }
    }

    Symbolized SymbolizeDump(const std::filesystem::path& dmp, const SymbolizeOptions& opt)
    {
        Symbolized out;

        // dbgeng.h declares every interface DECLSPEC_UUID, so __uuidof is the
        // idiom (UE: WindowsPlatformStackWalkExt.cpp:51-55) -- no hand-rolled
        // GUID and no IID_ linkage to get wrong.
        ComPtr<IDebugClient5> client;
        HRESULT hr = DebugCreate(__uuidof(IDebugClient5), reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) { out.engineError = "DebugCreate failed: " + Hex(hr); return out; }

        ComPtr<IDebugControl4>       control;
        ComPtr<IDebugSymbols3>       symbols;
        ComPtr<IDebugSystemObjects4> sys;
        if (FAILED(client.As(&control)) || FAILED(client.As(&symbols)) || FAILED(client.As(&sys)))
        { out.engineError = "dbgeng interfaces unavailable"; return out; }

        // UE's option set (WindowsPlatformStackWalkExt.cpp:74-95), set BEFORE
        // the dump opens: line info, nearest OMAP, fail on critical errors,
        // deferred loads, EXACT symbols (a GUID/age mismatch is "no symbols",
        // never a WRONG name -- a plausible wrong function is worse than an
        // offset), undecorated names. IGNORE_CVREC is the D5 seam on top: do
        // not follow the PDB path the linker embedded in the image, which is
        // the only way to hide a PDB on the desk that built it. NO_IMAGE_SEARCH
        // rides with it for the same reason -- without it dbghelp walks back
        // to the module's ORIGINAL directory, recorded in the dump, and finds
        // the very PDB the caller asked it not to consult.
        ULONG so = SYMOPT_LOAD_LINES | SYMOPT_OMAP_FIND_NEAREST | SYMOPT_FAIL_CRITICAL_ERRORS
                 | SYMOPT_DEFERRED_LOADS | SYMOPT_EXACT_SYMBOLS | SYMOPT_UNDNAME;
        if (opt.ignoreCvRecord) so |= SYMOPT_IGNORE_CVREC | SYMOPT_NO_IMAGE_SEARCH;
        symbols->SetSymbolOptions(so);

        hr = client->OpenDumpFileWide(dmp.c_str(), 0);
        if (FAILED(hr)) { out.engineError = "OpenDumpFile failed: " + Hex(hr); return out; }

        SessionEnder ender;
        ender.client = client.Get();

        hr = control->WaitForEvent(0, opt.waitForEventMs);   // UE: WaitForEvent(0, INFINITE), :568 -- ours is bounded
        if (FAILED(hr)) { out.engineError = "WaitForEvent failed: " + Hex(hr); return out; }
        out.engineAvailable = true;

        // The symbol path needs the module list, so it is set AFTER the dump
        // opens (UE: CrashDebugHelperWindows.cpp:30-33 -- InitSymbols, open,
        // SetSymbolPathsFromModules). --symbol-path REPLACES the whole search.
        const std::wstring symPath = opt.symbolPath.empty() ? DefaultSymbolPath(symbols.Get()) : ToWide(opt.symbolPath);
        symbols->SetSymbolPathWide(symPath.c_str());
        symbols->SetImagePathWide(symPath.c_str());
        out.symbolPath = ToUtf8(symPath);

        // The faulting thread, UE's way (:463-490): the STORED event's CONTEXT
        // is the exception context the host put in the dump, and
        // GetContextStackTrace walks from THAT -- not from whatever thread the
        // engine happens to land on, which for a minidump is thread 0. Every
        // Arcane dump carries a stored event: Diagnostics.cpp synthesizes an
        // exception record for hang and manual reports (this task), exactly as
        // UE does for a suspended thread (:1925-1966).
        std::vector<std::uint8_t> ctx(4096);
        ULONG eventType = 0, eventPid = 0, eventTid = 0, ctxUsed = 0;
        bool  haveEvent = SUCCEEDED(control->GetStoredEventInformation(&eventType, &eventPid, &eventTid,
                                                                       ctx.data(), static_cast<ULONG>(ctx.size()), &ctxUsed,
                                                                       nullptr, 0, nullptr))
                       && ctxUsed > 0;
        // R76, same class as the truncated strings above: SUCCEEDED admits
        // S_FALSE, and this call's S_FALSE means "your buffer was too small --
        // ctxUsed is what I NEEDED". Handing that number straight back as a
        // LENGTH would have GetContextStackTrace read past the end of `ctx`.
        // An x64 CONTEXT is ~1.2 KB against this 4 KB buffer, so it is the
        // extended-state machine of some future desk that would hit it --
        // silently, in the one process that must not be the thing that breaks.
        //
        // Grown and retried once rather than merely clamped: giving up here
        // costs the FAULTING-THREAD WALK, which is the whole feature, and the
        // required size is right there. A second overflow is refused outright
        // and the envelope's walked thread becomes the fallback
        // (PutFaultingFirst), which is exactly what that fallback is for.
        if (haveEvent && ctxUsed > ctx.size())
        {
            ctx.assign(ctxUsed, 0);
            ULONG again = 0;
            haveEvent = SUCCEEDED(control->GetStoredEventInformation(&eventType, &eventPid, &eventTid,
                                                                     ctx.data(), static_cast<ULONG>(ctx.size()), &again,
                                                                     nullptr, 0, nullptr))
                     && again > 0 && again <= ctx.size();
            ctxUsed = again;
        }
        if (haveEvent)
        {
            SymThread t;
            t.faulting = true;

            // R77: these two decide the LABEL on the faulting thread, and a
            // correct stack under a wrong id is worse than one that admits it
            // does not know. Unchecked, a failed SetCurrentThreadId leaves the
            // engine on whatever thread it was already on and
            // GetCurrentThreadSystemId then cheerfully reports THAT one. Both
            // are checked; on failure the id stays 0, which the text prints as
            // "<unknown>" rather than as "thread 0". The FRAMES are unaffected
            // either way -- GetContextStackTrace walks from the stored context
            // that is passed to it explicitly, not from the current thread.
            ULONG sysId = 0;
            if (SUCCEEDED(sys->SetCurrentThreadId(eventTid)) && SUCCEEDED(sys->GetCurrentThreadSystemId(&sysId)))
                t.systemId = sysId;

            // R112: this walk -- the faulting thread only -- gets the deep
            // 8192-frame cap; other threads below still use
            // maxFramesPerThread. FrameContexts is nullptr/0 (we do not ask
            // dbgeng for per-frame contexts), so this vector is the only
            // buffer that scales with the cap, and it is a std::vector, not a
            // fixed-size array -- 8192 * sizeof(DEBUG_STACK_FRAME) is a heap
            // allocation, not a stack overrun. (UE's MaxFramesSize concern is
            // for its fixed local array; not applicable to this shape.)
            std::vector<DEBUG_STACK_FRAME> frames(opt.maxFramesFaultingThread);
            ULONG                          filled = 0;
            if (SUCCEEDED(control->GetContextStackTrace(ctx.data(), ctxUsed, frames.data(), static_cast<ULONG>(frames.size()),
                                                        nullptr, 0, 0, &filled)))
            {
                for (ULONG k = 0; k < filled; ++k)
                    t.frames.push_back(Resolve(symbols.Get(), frames[k].InstructionOffset));
                // R78: dbgeng stops at the buffer, so a full buffer means "the
                // stack may go deeper" -- conservatively true even for a stack
                // that happens to be exactly this deep, which is the right way
                // round for a truncation marker.
                t.framesTruncated = filled == frames.size();
            }
            out.threads.push_back(std::move(t));
        }

        // Every OTHER thread from its own saved context (what windbg's ~*k
        // does). This is beyond what UE's client walks -- its other threads
        // come from the in-process portable capture -- so it is best-effort: a
        // thread that fails to WALK is listed with no frames rather than
        // failing the report.
        //
        // R77 made the code match that sentence, in the one direction that is
        // honest. There were two `continue`s dropping a thread silently, and
        // they are NOT the same failure:
        //
        //  - GetThreadIdsByIndex failing means we have no IDENTITY for the
        //    thread. There is nothing to list -- "--- thread <unknown>" with no
        //    frames would be a row of pure noise -- so it still skips.
        //  - SetCurrentThreadId failing means we HAVE the system id and merely
        //    cannot walk it. That is exactly the case the comment promised, so
        //    it now lists the thread with no frames. A thread that exists and
        //    could not be walked is a fact about the dump; deleting it from the
        //    report hides that a thread was there at all.
        ULONG count = 0;
        sys->GetNumberThreads(&count);
        out.threadsTruncated = count > opt.maxThreads;   // R78
        for (ULONG i = 0; i < count && i < opt.maxThreads; ++i)
        {
            ULONG engineId = 0, systemId = 0;
            if (FAILED(sys->GetThreadIdsByIndex(i, 1, &engineId, &systemId))) continue;
            if (haveEvent && engineId == eventTid) continue;

            SymThread t;
            t.systemId = systemId;

            if (SUCCEEDED(sys->SetCurrentThreadId(engineId)))
            {
                std::vector<DEBUG_STACK_FRAME> frames(opt.maxFramesPerThread);
                ULONG                          filled = 0;
                if (SUCCEEDED(control->GetStackTrace(0, 0, 0, frames.data(), static_cast<ULONG>(frames.size()), &filled)))
                {
                    for (ULONG k = 0; k < filled; ++k)
                        t.frames.push_back(Resolve(symbols.Get(), frames[k].InstructionOffset));
                    t.framesTruncated = filled == frames.size();   // R78
                }
            }
            out.threads.push_back(std::move(t));
        }

        return out;   // ~SessionEnder detaches; the ComPtrs release in reverse order
    }
}
