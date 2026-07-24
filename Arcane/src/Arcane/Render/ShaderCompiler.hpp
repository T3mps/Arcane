#pragma once

// ShaderCompiler: the runtime HLSL compile service (Slice 2 of the shader-editor
// arc). In-process dxcompiler.dll (IDxcCompiler3) -- the vendored trio at
// ThirdParty/tools/dxc is copied beside every hosting exe, so runtime compiles
// cost zero new dependencies. One request compiles DXIL and SPIR-V as two
// independent Compile() calls over the same in-memory source (per-target
// results; a backend publishes only on ITS target's success). Work runs on a
// dedicated worker thread; Submit/Poll/Drain are MAIN-THREAD-ONLY, and only the
// Drain() site may touch NVRHI (createShader + Generation bump live with the
// caller -- NVRHI is not free-threaded). Debounce: Submit coalesces by
// coalesceKey; Poll dispatches a job once its quiet window elapses; a result
// superseded by a newer Submit for the same key is dropped at Drain. Diags are
// parsed ONCE at the boundary (Clang grammar `file:line:col: sev: msg` -- DXC's
// native form) into structured ShaderDiag records; warnings are parsed even on
// success. The Compile call is SEH-guarded (the compiler is untrusted input);
// a crash fails the job, never unwinds the engine. Results are cached by
// content hash (source + args + the dxc DLL bytes -- toolchain bumps
// auto-invalidate). CompileNow is the synchronous in-line variant (tests,
// tools); it shares the cache.

#include <Arcane/Base/Api.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane
{
    enum class ShaderDiagSeverity : std::uint8_t { Note, Warning, Error };

    // One structured diagnostic, parsed from DXC's Clang-grammar text output.
    struct ShaderDiag
    {
        std::string file;         // virtual source name (the request's debugName)
        int line = 0;             // 1-based; 0 = no source location
        int col = 0;
        ShaderDiagSeverity severity = ShaderDiagSeverity::Error;
        std::string message;
        std::string sourceLine;   // the offending source line, when DXC echoed it
        std::string caret;        // the ^~~~ marker line, when present
    };

    // Parse DXC's stderr-style diagnostic text into records. Exposed for tests
    // and for any tool that captures raw dxc output. Grammar per line:
    // `file:line:col: {note|warning|error|fatal error}: message`, optionally
    // followed by the echoed source line and a caret line. Location-less
    // `error: message` lines are captured too.
    ARCANE_API std::vector<ShaderDiag> ParseDxcDiagnostics(std::string_view text);

    struct ShaderCompileRequest
    {
        std::string debugName = "shader.hlsl";   // virtual name shown in diags
        std::string sourceUtf8;
        std::string entry = "ps_main";           // ShaderConventions.hpp
        std::string profile = "ps_6_5";
        std::vector<std::pair<std::string, std::string>> defines;   // -D name=value
        // Jobs sharing a nonzero key coalesce: a newer Submit replaces a pending
        // older one, and stale in-flight results are dropped at Drain. Use the
        // material/document identity. 0 = never coalesced.
        std::uint64_t coalesceKey = 0;
    };

    struct ShaderTargetResult
    {
        bool succeeded = false;
        std::vector<std::uint8_t> bytecode;   // empty on failure
        std::vector<ShaderDiag> diags;        // warnings present even on success
        std::string reproCmdLine;             // offline repro: dxc.exe <args>
    };

    struct ShaderCompileResult
    {
        std::uint64_t jobId = 0;
        std::uint64_t coalesceKey = 0;
        std::uint64_t contentHash = 0;   // cache key: source + args + toolchain bytes
        std::string debugName;
        bool fromCache = false;
        bool crashed = false;            // SEH caught inside dxcompiler
        ShaderTargetResult dxil;
        ShaderTargetResult spirv;

        bool AllSucceeded() const { return dxil.succeeded && spirv.succeeded; }
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API ShaderCompiler
    {
    public:
        ShaderCompiler();
        ~ShaderCompiler();
        ShaderCompiler(const ShaderCompiler&) = delete;
        ShaderCompiler& operator=(const ShaderCompiler&) = delete;

        // Load dxcompiler.dll + dxil.dll (exe directory first, then the normal
        // search path), hash their bytes into the cache key, start the worker.
        // False (and IsAvailable()==false) when the compiler DLL cannot be
        // loaded; a missing dxil.dll only warns (DXIL comes out unsigned).
        // debounceSeconds = the quiet window Poll enforces per coalesceKey.
        bool Initialize(double debounceSeconds = 0.2);
        void Shutdown();
        bool IsAvailable() const { return m_available; }

        // Queue a compile (main thread). `now` is the caller's monotonic clock in
        // seconds -- Submit stamps the quiet window with it, Poll compares against
        // it (injected so tests drive time). Returns the job id.
        std::uint64_t Submit(ShaderCompileRequest req, double now);

        // Dispatch pending jobs whose quiet window elapsed (main thread). Cache
        // hits complete immediately without touching the worker.
        void Poll(double now);

        // Pop finished results (main thread). Superseded results are dropped;
        // fresh ones land in the cache and update LastGood for their key.
        std::vector<ShaderCompileResult> Drain();

        // True when nothing is pending, in flight, or waiting to be drained.
        bool IsIdle() const;

        // Completed results not yet picked up by Drain (worker-done + cache hits).
        std::size_t UndrainedCount() const;

        // Synchronous compile on the calling thread (main thread; tests/tools).
        // Shares the content-hash cache with the async path.
        ShaderCompileResult CompileNow(const ShaderCompileRequest& req);

        // Last fully-successful result for a coalesce key -- the "last-good stays
        // bound" seam consumers render from while a newer compile is in flight or
        // failing. Null until a compile for the key succeeds on both targets.
        const ShaderCompileResult* LastGood(std::uint64_t coalesceKey) const;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
        bool m_available = false;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
