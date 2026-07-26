#include <Arcane/Render/ShaderCompiler.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderConventions.hpp>

#include <charconv>
#include <cstring>

namespace Arcane
{
    // ------------------------------------------------------------------------
    // Diagnostic parsing (portable, pure -- unit-tested directly).
    // ------------------------------------------------------------------------
    namespace
    {
        struct SevToken
        {
            const char* token;
            ShaderDiagSeverity sev;
        };
        // Longest first: ": fatal error: " must win over ": error: ".
        constexpr SevToken kSevTokens[] = {
            { ": fatal error: ", ShaderDiagSeverity::Error },
            { ": error: ",       ShaderDiagSeverity::Error },
            { ": warning: ",     ShaderDiagSeverity::Warning },
            { ": note: ",        ShaderDiagSeverity::Note },
        };
        constexpr SevToken kBareSevTokens[] = {
            { "fatal error: ", ShaderDiagSeverity::Error },
            { "error: ",       ShaderDiagSeverity::Error },
            { "warning: ",     ShaderDiagSeverity::Warning },
            { "note: ",        ShaderDiagSeverity::Note },
        };

        bool ParseInt(std::string_view s, int& out)
        {
            if (s.empty())
                return false;
            const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
            return r.ec == std::errc() && r.ptr == s.data() + s.size() && out > 0;
        }

        // `file:line:col: sev: message` or a location-less `sev: message`.
        bool ParseDiagHeader(std::string_view line, ShaderDiag& out)
        {
            for (const SevToken& t : kSevTokens)
            {
                const std::size_t pos = line.find(t.token);
                if (pos == std::string_view::npos)
                    continue;

                std::string_view prefix = line.substr(0, pos);
                out.severity = t.sev;
                out.message = std::string(line.substr(pos + std::strlen(t.token)));

                // From the right: :col, :line, rest = file (file may contain ':').
                const std::size_t colSep = prefix.rfind(':');
                const std::size_t lineSep = colSep != std::string_view::npos && colSep > 0
                                                ? prefix.rfind(':', colSep - 1)
                                                : std::string_view::npos;
                int lineNo = 0;
                int colNo = 0;
                if (colSep != std::string_view::npos && lineSep != std::string_view::npos &&
                    ParseInt(prefix.substr(colSep + 1), colNo) &&
                    ParseInt(prefix.substr(lineSep + 1, colSep - lineSep - 1), lineNo))
                {
                    out.file = std::string(prefix.substr(0, lineSep));
                    out.line = lineNo;
                    out.col = colNo;
                }
                else
                {
                    out.file = std::string(prefix);
                    out.line = 0;
                    out.col = 0;
                }
                return true;
            }

            for (const SevToken& t : kBareSevTokens)
            {
                if (line.substr(0, std::strlen(t.token)) == t.token)
                {
                    out.severity = t.sev;
                    out.message = std::string(line.substr(std::strlen(t.token)));
                    out.file.clear();
                    out.line = 0;
                    out.col = 0;
                    return true;
                }
            }
            return false;
        }

        bool IsCaretLine(std::string_view line)
        {
            bool sawCaret = false;
            for (char c : line)
            {
                if (c == '^')
                    sawCaret = true;
                else if (c != ' ' && c != '\t' && c != '~')
                    return false;
            }
            return sawCaret;
        }

        std::vector<std::string_view> SplitLines(std::string_view text)
        {
            std::vector<std::string_view> lines;
            std::size_t start = 0;
            while (start <= text.size())
            {
                std::size_t end = text.find('\n', start);
                if (end == std::string_view::npos)
                    end = text.size();
                std::string_view line = text.substr(start, end - start);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                lines.push_back(line);
                if (end == text.size())
                    break;
                start = end + 1;
            }
            return lines;
        }
    }

    std::vector<ShaderDiag> ParseDxcDiagnostics(std::string_view text)
    {
        std::vector<ShaderDiag> diags;
        const auto lines = SplitLines(text);

        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            ShaderDiag d;
            if (!ParseDiagHeader(lines[i], d))
                continue;

            // DXC echoes the offending source line and a caret line right after a
            // located diagnostic; capture the pair only when the caret confirms it.
            if (d.line > 0 && i + 2 < lines.size() && IsCaretLine(lines[i + 2]))
            {
                ShaderDiag probe;
                if (!ParseDiagHeader(lines[i + 1], probe))
                {
                    d.sourceLine = std::string(lines[i + 1]);
                    d.caret = std::string(lines[i + 2]);
                    i += 2;
                }
            }
            diags.push_back(std::move(d));
        }
        return diags;
    }
}

#if !defined(_WIN32)

// Non-Windows stub: the compile service is Windows-only until the Linux port
// milestone (libdxcompiler.so lands there).
namespace Arcane
{
    struct ShaderCompiler::Impl {};
    ShaderCompiler::ShaderCompiler() = default;
    ShaderCompiler::~ShaderCompiler() = default;
    bool ShaderCompiler::Initialize(double) { return false; }
    void ShaderCompiler::Shutdown() {}
    std::uint64_t ShaderCompiler::Submit(ShaderCompileRequest, double) { return 0; }
    void ShaderCompiler::Poll(double) {}
    std::vector<ShaderCompileResult> ShaderCompiler::Drain() { return {}; }
    bool ShaderCompiler::IsIdle() const { return true; }
    std::size_t ShaderCompiler::UndrainedCount() const { return 0; }
    ShaderCompileResult ShaderCompiler::CompileNow(const ShaderCompileRequest&) { return {}; }
    const ShaderCompileResult* ShaderCompiler::LastGood(std::uint64_t) const { return nullptr; }
}

#else

#include <windows.h>

#include <unknwn.h>   // WIN32_LEAN_AND_MEAN strips COM from windows.h; dxcapi.h needs IUnknown

#include <dxcapi.h>
#include <wrl/client.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Arcane
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
        constexpr std::uint64_t kFnvPrime = 1099511628211ull;

        std::uint64_t Fnv64(std::uint64_t h, const void* data, std::size_t size) noexcept
        {
            const auto* p = static_cast<const std::uint8_t*>(data);
            for (std::size_t i = 0; i < size; ++i)
            {
                h ^= p[i];
                h *= kFnvPrime;
            }
            return h;
        }

        std::uint64_t Fnv64Str(std::uint64_t h, std::string_view s) noexcept
        {
            return Fnv64(h, s.data(), s.size());
        }

        std::wstring Widen(std::string_view s)
        {
            std::wstring w;
            w.reserve(s.size());
            for (char c : s)
                w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            return w;
        }

        std::filesystem::path ExeDirectory()
        {
            wchar_t buf[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            return std::filesystem::path(buf).parent_path();
        }

        std::uint64_t HashFileBytes(std::uint64_t h, const std::filesystem::path& p)
        {
            std::ifstream f(p, std::ios::binary);
            if (!f)
                return h;
            char buf[64 * 1024];
            while (f.read(buf, sizeof(buf)) || f.gcount() > 0)
                h = Fnv64(h, buf, static_cast<std::size_t>(f.gcount()));
            return h;
        }

        // The one SEH frame: treat dxcompiler as untrusted input -- a compiler
        // crash fails the job, it never unwinds the engine. No C++ objects with
        // destructors may live in this function (C2712).
        int CompileGuarded(IDxcCompiler3* compiler, const DxcBuffer* source,
                           LPCWSTR* argv, UINT32 argc, IDxcResult** outResult,
                           HRESULT* outHr) noexcept
        {
            __try
            {
                *outHr = compiler->Compile(source, argv, argc, nullptr, IID_PPV_ARGS(outResult));
                return 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 1;
            }
        }

        // Build the CLI-shaped argv for one target (argv[0] = the virtual source
        // name, exactly how UE feeds IDxcCompiler3 -- DXC uses it for diagnostics).
        std::vector<std::string> BuildArgs(const ShaderCompileRequest& req, bool spirv)
        {
            std::vector<std::string> args;
            args.push_back(req.debugName);
            args.push_back("-T");
            args.push_back(req.profile);
            args.push_back("-E");
            args.push_back(req.entry);
            for (const auto& [name, value] : req.defines)
            {
                args.push_back("-D");
                args.push_back(value.empty() ? name : name + "=" + value);
            }
            if (spirv)
            {
                for (std::size_t i = 0; i < kSpirvArgCount; ++i)
                    args.push_back(kSpirvArgs[i]);
            }
            return args;
        }

        std::string JoinRepro(const std::vector<std::string>& args)
        {
            std::string cmd = "dxc.exe";
            for (const std::string& a : args)
            {
                cmd += ' ';
                cmd += a;
            }
            return cmd;
        }

        ShaderTargetResult CompileTarget(IDxcCompiler3* compiler,
                                         const ShaderCompileRequest& req, bool spirv,
                                         bool& crashed, bool& environmental)
        {
            ShaderTargetResult out;
            const std::vector<std::string> args = BuildArgs(req, spirv);
            out.reproCmdLine = JoinRepro(args);

            std::vector<std::wstring> wide;
            wide.reserve(args.size());
            for (const std::string& a : args)
                wide.push_back(Widen(a));
            std::vector<LPCWSTR> argv;
            argv.reserve(wide.size());
            for (const std::wstring& w : wide)
                argv.push_back(w.c_str());

            DxcBuffer source{};
            source.Ptr = req.sourceUtf8.data();
            source.Size = req.sourceUtf8.size();
            source.Encoding = DXC_CP_UTF8;

            IDxcResult* rawResult = nullptr;
            HRESULT hr = E_FAIL;
            if (CompileGuarded(compiler, &source, argv.data(),
                               static_cast<UINT32>(argv.size()), &rawResult, &hr) != 0)
            {
                crashed = true;
                ShaderDiag d;
                d.file = req.debugName;
                d.severity = ShaderDiagSeverity::Error;
                d.message = "shader compiler crashed (structured exception inside dxcompiler.dll)";
                out.diags.push_back(std::move(d));
                return out;
            }
            ComPtr<IDxcResult> result;
            result.Attach(rawResult);

            if (FAILED(hr) || !result)
            {
                environmental = true;   // toolchain hiccup, not a content verdict
                ShaderDiag d;
                d.file = req.debugName;
                d.severity = ShaderDiagSeverity::Error;
                d.message = "IDxcCompiler3::Compile failed (hr=" + std::to_string(hr) + ")";
                out.diags.push_back(std::move(d));
                return out;
            }

            // Read the error buffer EVEN ON SUCCESS -- warnings live there.
            ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
                errors && errors->GetStringLength() > 0)
            {
                out.diags = ParseDxcDiagnostics(
                    std::string_view(errors->GetStringPointer(), errors->GetStringLength()));
            }

            HRESULT status = E_FAIL;
            result->GetStatus(&status);
            if (FAILED(status))
                return out;

            ComPtr<IDxcBlob> object;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) &&
                object && object->GetBufferSize() > 0)
            {
                const auto* p = static_cast<const std::uint8_t*>(object->GetBufferPointer());
                out.bytecode.assign(p, p + object->GetBufferSize());
                out.succeeded = true;
            }
            else
            {
                ShaderDiag d;
                d.file = req.debugName;
                d.severity = ShaderDiagSeverity::Error;
                d.message = "compile reported success but produced no object blob";
                out.diags.push_back(std::move(d));
            }
            return out;
        }
    }

    struct ShaderCompiler::Impl
    {
        struct Job
        {
            std::uint64_t jobId = 0;
            ShaderCompileRequest req;
            std::uint64_t contentHash = 0;
        };
        struct Pending
        {
            std::uint64_t jobId = 0;
            double readyAt = 0.0;
            ShaderCompileRequest req;
        };

        // Toolchain (set once in Initialize).
        HMODULE hDxil = nullptr;
        HMODULE hCompiler = nullptr;   // stays loaded for process lifetime (COM DLL unload is unsafe)
        DxcCreateInstanceProc createInstance = nullptr;
        std::uint64_t toolchainHash = 0;
        double debounce = 0.2;

        // Main-thread state (Submit/Poll/Drain/CompileNow are main-thread-only).
        std::uint64_t nextJobId = 1;
        std::unordered_map<std::uint64_t, Pending> pendingByKey;   // coalesceKey != 0
        std::deque<Pending> pendingKeyless;
        std::unordered_map<std::uint64_t, std::uint64_t> latestJobForKey;
        std::unordered_map<std::uint64_t, ShaderCompileResult> cache;      // contentHash ->
        std::unordered_map<std::uint64_t, ShaderCompileResult> lastGood;   // coalesceKey ->
        std::vector<ShaderCompileResult> readyMain;   // cache-hit completions
        ComPtr<IDxcCompiler3> mainCompiler;           // CompileNow's instance

        // Worker state.
        std::thread worker;
        std::mutex mx;
        std::condition_variable cv;
        std::deque<Job> queue;
        std::vector<ShaderCompileResult> done;
        int inflight = 0;      // guarded by mx
        bool stop = false;

        std::uint64_t ContentHash(const ShaderCompileRequest& req) const
        {
            std::uint64_t h = kFnvOffset;
            h = Fnv64Str(h, req.sourceUtf8);
            h = Fnv64Str(h, req.entry);
            h = Fnv64Str(h, req.profile);
            for (const auto& [name, value] : req.defines)
            {
                h = Fnv64Str(h, name);
                h = Fnv64Str(h, value);
            }
            for (std::size_t i = 0; i < kSpirvArgCount; ++i)
                h = Fnv64Str(h, kSpirvArgs[i]);
            h = Fnv64(h, &toolchainHash, sizeof(toolchainHash));
            return h;
        }

        ShaderCompileResult RunJob(IDxcCompiler3* compiler, const Job& job)
        {
            ShaderCompileResult r;
            r.jobId = job.jobId;
            r.coalesceKey = job.req.coalesceKey;
            r.contentHash = job.contentHash;
            r.debugName = job.req.debugName;
            r.dxil = CompileTarget(compiler, job.req, /*spirv=*/false, r.crashed, r.environmental);
            if (r.crashed)
            {
                // The instance may be corrupt after the SEH crash -- never run
                // the second target through it (the caller drops the instance).
                ShaderDiag d;
                d.file = job.req.debugName;
                d.severity = ShaderDiagSeverity::Error;
                d.message = "SPIR-V compile skipped (compiler crashed on the DXIL target)";
                r.spirv.diags.push_back(std::move(d));
                return r;
            }
            r.spirv = CompileTarget(compiler, job.req, /*spirv=*/true, r.crashed, r.environmental);
            return r;
        }

        void WorkerMain()
        {
            ComPtr<IDxcCompiler3> compiler;   // one instance per thread
            for (;;)
            {
                Job job;
                {
                    std::unique_lock lk(mx);
                    cv.wait(lk, [this] { return stop || !queue.empty(); });
                    if (stop)
                        return;
                    job = std::move(queue.front());
                    queue.pop_front();
                    ++inflight;
                }

                if (!compiler)
                    createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));

                ShaderCompileResult r = compiler
                    ? RunJob(compiler.Get(), job)
                    : ShaderCompileResult{};   // instance creation failed
                if (!compiler)
                {
                    r.jobId = job.jobId;
                    r.coalesceKey = job.req.coalesceKey;
                    r.contentHash = job.contentHash;
                    r.debugName = job.req.debugName;
                    r.environmental = true;
                    ShaderDiag d;
                    d.file = job.req.debugName;
                    d.message = "DxcCreateInstance failed on the compile worker";
                    r.dxil.diags.push_back(d);
                    r.spirv.diags.push_back(std::move(d));
                }
                if (r.crashed)
                    compiler.Reset();   // recreate a fresh instance for the next job

                {
                    std::lock_guard lk(mx);
                    done.push_back(std::move(r));
                    --inflight;
                }
            }
        }

        void Dispatch(Pending&& p)
        {
            const std::uint64_t hash = ContentHash(p.req);
            if (auto it = cache.find(hash); it != cache.end())
            {
                ShaderCompileResult r = it->second;
                r.jobId = p.jobId;
                r.coalesceKey = p.req.coalesceKey;
                r.fromCache = true;
                readyMain.push_back(std::move(r));
                return;
            }
            {
                std::lock_guard lk(mx);
                queue.push_back(Job{ p.jobId, std::move(p.req), hash });
            }
            cv.notify_one();
        }

        // Cache + last-good bookkeeping. Crashed/environmental results are never
        // cached (no content verdict). updateLastGood=false for superseded
        // results: their bytecode is still cache-worthy (undo back to a
        // just-compiled state must hit), but lastGood stays latest-only.
        void Absorb(const ShaderCompileResult& r, bool updateLastGood)
        {
            if (!r.fromCache && !r.crashed && !r.environmental)
            {
                ShaderCompileResult stored = r;
                stored.fromCache = false;
                cache[r.contentHash] = std::move(stored);
            }
            if (updateLastGood && r.coalesceKey != 0 && r.AllSucceeded())
                lastGood[r.coalesceKey] = r;
        }
    };

    ShaderCompiler::ShaderCompiler() : m_impl(new Impl) {}

    ShaderCompiler::~ShaderCompiler()
    {
        Shutdown();
        delete m_impl;
    }

    bool ShaderCompiler::Initialize(double debounceSeconds)
    {
        Impl& im = *m_impl;
        if (m_available)
            return true;
        im.debounce = debounceSeconds;

        // Vendored trio beside the exe first (the postbuild copies), then the
        // regular search path. dxil.dll loads FIRST so dxcompiler's validator
        // finds it (missing validator = unsigned DXIL, warn but continue).
        const std::filesystem::path exeDir = ExeDirectory();
        const std::filesystem::path dxilPath = exeDir / L"dxil.dll";
        const std::filesystem::path compilerPath = exeDir / L"dxcompiler.dll";

        im.hDxil = LoadLibraryW(dxilPath.c_str());
        if (!im.hDxil)
            im.hDxil = LoadLibraryW(L"dxil.dll");
        if (!im.hDxil)
            ARC_WARN("ShaderCompiler: dxil.dll not found -- DXIL output will be unsigned");

        im.hCompiler = LoadLibraryW(compilerPath.c_str());
        if (!im.hCompiler)
            im.hCompiler = LoadLibraryW(L"dxcompiler.dll");
        if (!im.hCompiler)
        {
            ARC_ERROR("ShaderCompiler: dxcompiler.dll not found -- runtime compiles unavailable");
            return false;
        }

        im.createInstance = reinterpret_cast<DxcCreateInstanceProc>(
            GetProcAddress(im.hCompiler, "DxcCreateInstance"));
        if (!im.createInstance)
        {
            ARC_ERROR("ShaderCompiler: DxcCreateInstance export missing from dxcompiler.dll");
            return false;
        }

        // Toolchain bytes fold into every content hash -- a dxc upgrade
        // auto-invalidates the whole cache (UE's DXCWrapper trick).
        wchar_t modulePath[MAX_PATH] = {};
        GetModuleFileNameW(im.hCompiler, modulePath, MAX_PATH);
        std::uint64_t th = kFnvOffset;
        th = HashFileBytes(th, modulePath);
        if (im.hDxil)
        {
            GetModuleFileNameW(im.hDxil, modulePath, MAX_PATH);
            th = HashFileBytes(th, modulePath);
        }
        im.toolchainHash = th;

        im.stop = false;
        im.worker = std::thread([this] { m_impl->WorkerMain(); });
        m_available = true;
        ARC_INFO("ShaderCompiler: in-process dxc ready (debounce {:.0f} ms)", debounceSeconds * 1000.0);
        return true;
    }

    void ShaderCompiler::Shutdown()
    {
        Impl& im = *m_impl;
        if (im.worker.joinable())
        {
            {
                std::lock_guard lk(im.mx);
                im.stop = true;
            }
            im.cv.notify_all();
            im.worker.join();
        }
        im.mainCompiler.Reset();
        // hCompiler/hDxil stay loaded on purpose: unloading a COM-style DLL that
        // may still own module-static state is a classic shutdown crash; the OS
        // reclaims them at process exit.
        m_available = false;
    }

    std::uint64_t ShaderCompiler::Submit(ShaderCompileRequest req, double now)
    {
        Impl& im = *m_impl;
        if (!m_available)
            return 0;

        const std::uint64_t jobId = im.nextJobId++;
        Impl::Pending p{ jobId, now + im.debounce, std::move(req) };

        if (p.req.coalesceKey != 0)
        {
            im.latestJobForKey[p.req.coalesceKey] = jobId;
            im.pendingByKey[p.req.coalesceKey] = std::move(p);   // newer replaces pending older
        }
        else
        {
            im.pendingKeyless.push_back(std::move(p));
        }
        return jobId;
    }

    void ShaderCompiler::Poll(double now)
    {
        Impl& im = *m_impl;
        if (!m_available)
            return;

        for (auto it = im.pendingByKey.begin(); it != im.pendingByKey.end();)
        {
            if (now >= it->second.readyAt)
            {
                Impl::Pending p = std::move(it->second);
                it = im.pendingByKey.erase(it);
                im.Dispatch(std::move(p));
            }
            else
            {
                ++it;
            }
        }
        while (!im.pendingKeyless.empty() && now >= im.pendingKeyless.front().readyAt)
        {
            Impl::Pending p = std::move(im.pendingKeyless.front());
            im.pendingKeyless.pop_front();
            im.Dispatch(std::move(p));
        }
    }

    std::vector<ShaderCompileResult> ShaderCompiler::Drain()
    {
        Impl& im = *m_impl;
        std::vector<ShaderCompileResult> raw = std::move(im.readyMain);
        im.readyMain.clear();
        {
            std::lock_guard lk(im.mx);
            raw.insert(raw.end(), std::make_move_iterator(im.done.begin()),
                       std::make_move_iterator(im.done.end()));
            im.done.clear();
        }

        std::vector<ShaderCompileResult> out;
        out.reserve(raw.size());
        for (ShaderCompileResult& r : raw)
        {
            // Superseded: a newer Submit for the same key exists -- drop it (the
            // newer job's result is the one the consumer must see). Its bytecode
            // still enters the content cache so undoing back to this state is a
            // cache hit, but lastGood stays latest-only.
            if (r.coalesceKey != 0)
            {
                const auto it = im.latestJobForKey.find(r.coalesceKey);
                if (it != im.latestJobForKey.end() && it->second != r.jobId)
                {
                    im.Absorb(r, /*updateLastGood=*/false);
                    continue;
                }
            }
            im.Absorb(r, /*updateLastGood=*/true);
            out.push_back(std::move(r));
        }
        return out;
    }

    bool ShaderCompiler::IsIdle() const
    {
        Impl& im = *m_impl;
        if (!im.pendingByKey.empty() || !im.pendingKeyless.empty() || !im.readyMain.empty())
            return false;
        std::lock_guard lk(im.mx);
        return im.queue.empty() && im.inflight == 0 && im.done.empty();
    }

    std::size_t ShaderCompiler::UndrainedCount() const
    {
        Impl& im = *m_impl;
        std::lock_guard lk(im.mx);
        return im.readyMain.size() + im.done.size();
    }

    ShaderCompileResult ShaderCompiler::CompileNow(const ShaderCompileRequest& req)
    {
        Impl& im = *m_impl;
        ShaderCompileResult r;
        if (!m_available)
        {
            r.debugName = req.debugName;
            return r;
        }

        const std::uint64_t hash = im.ContentHash(req);
        const std::uint64_t jobId = im.nextJobId++;
        if (auto it = im.cache.find(hash); it != im.cache.end())
        {
            r = it->second;
            r.jobId = jobId;
            r.coalesceKey = req.coalesceKey;
            r.fromCache = true;
            im.Absorb(r, /*updateLastGood=*/true);   // sync cache hit still refreshes LastGood
            return r;
        }

        if (!im.mainCompiler)
            im.createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&im.mainCompiler));
        if (!im.mainCompiler)
        {
            r.debugName = req.debugName;
            r.environmental = true;
            return r;
        }

        r = im.RunJob(im.mainCompiler.Get(), Impl::Job{ jobId, req, hash });
        if (r.crashed)
            im.mainCompiler.Reset();   // possibly-corrupt instance; recreate next call
        im.Absorb(r, /*updateLastGood=*/true);
        return r;
    }

    const ShaderCompileResult* ShaderCompiler::LastGood(std::uint64_t coalesceKey) const
    {
        const auto it = m_impl->lastGood.find(coalesceKey);
        return it != m_impl->lastGood.end() ? &it->second : nullptr;
    }
}

#endif
