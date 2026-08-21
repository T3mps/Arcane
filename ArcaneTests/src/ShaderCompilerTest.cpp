// Runtime shader compile service (Slice 2 of the shader-editor arc): in-process
// dxcompiler.dll, dual DXIL+SPIR-V targets from one source, structured diag
// parsing (Clang grammar), content-hash cache, debounce/coalesce, superseded
// drop. CPU-only: no device anywhere in this file.
// The dxc DLLs are copied beside the test exe by the premake postbuild.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

using namespace Arcane;

namespace
{
    constexpr const char* kGoodPs = R"(
struct Varyings { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 ps_main(Varyings v) : SV_Target
{
    return float4(v.uv, 0.0, 1.0);
}
)";

    // 'float2 = float3' triggers the implicit-truncation warning; compile still succeeds.
    constexpr const char* kWarnPs = R"(
struct Varyings { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 ps_main(Varyings v) : SV_Target
{
    float2 t = float3(v.uv, 1.0);
    return float4(t, 0.0, 1.0);
}
)";

    constexpr const char* kBadPs = R"(
struct Varyings { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 ps_main(Varyings v) : SV_Target
{
    return bogus_identifier;
}
)";

    ShaderCompileRequest MakeRequest(const char* source, std::uint64_t key = 0)
    {
        ShaderCompileRequest req;
        req.debugName = "test_material.hlsl";
        req.sourceUtf8 = source;
        req.entry = kPsEntry;
        req.profile = kPsProfile;
        req.coalesceKey = key;
        return req;
    }

    bool HasDxbcMagic(const std::vector<std::uint8_t>& bytes)
    {
        return bytes.size() >= 4 && std::memcmp(bytes.data(), "DXBC", 4) == 0;
    }

    bool HasSpirvMagic(const std::vector<std::uint8_t>& bytes)
    {
        std::uint32_t magic = 0;
        if (bytes.size() < sizeof(magic))
            return false;
        std::memcpy(&magic, bytes.data(), sizeof(magic));
        return magic == 0x07230203u;
    }

    // Bounded wait until at least `n` completed results sit undrained.
    void WaitUntilUndrained(ShaderCompiler& sc, std::size_t n)
    {
        for (int i = 0; i < 2000 && sc.UndrainedCount() < n; ++i)   // 10 s ceiling
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Waits until at least one result is drainable, draining as it goes.
    std::vector<ShaderCompileResult> DrainBlocking(ShaderCompiler& sc)
    {
        std::vector<ShaderCompileResult> out;
        for (int i = 0; i < 2000 && out.empty(); ++i)
        {
            out = sc.Drain();
            if (out.empty())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return out;
    }
}

TEST_CASE("ParseDxcDiagnostics handles located, bare, and follow-line forms", "[shadercompile]")
{
    const char* text =
        "mat.hlsl:5:12: error: use of undeclared identifier 'bogus'\n"
        "    return bogus;\n"
        "           ^~~~~\n"
        "mat.hlsl:9:3: warning: implicit truncation of vector type\n"
        "error: validation errors\n"
        "note: some note text\n";

    const auto diags = ParseDxcDiagnostics(text);
    REQUIRE(diags.size() == 4);

    CHECK(diags[0].file == "mat.hlsl");
    CHECK(diags[0].line == 5);
    CHECK(diags[0].col == 12);
    CHECK(diags[0].severity == ShaderDiagSeverity::Error);
    CHECK(diags[0].message == "use of undeclared identifier 'bogus'");
    CHECK(diags[0].sourceLine == "    return bogus;");
    CHECK(diags[0].caret == "           ^~~~~");

    CHECK(diags[1].line == 9);
    CHECK(diags[1].severity == ShaderDiagSeverity::Warning);
    CHECK(diags[1].sourceLine.empty());   // no caret pair followed

    CHECK(diags[2].file.empty());
    CHECK(diags[2].line == 0);
    CHECK(diags[2].severity == ShaderDiagSeverity::Error);
    CHECK(diags[2].message == "validation errors");

    CHECK(diags[3].severity == ShaderDiagSeverity::Note);
}

TEST_CASE("ShaderCompiler compiles one source to both targets in-process", "[shadercompile]")
{
    ShaderCompiler sc;
    REQUIRE(sc.Initialize(/*debounceSeconds=*/0.0));
    REQUIRE(sc.IsAvailable());

    const ShaderCompileResult r = sc.CompileNow(MakeRequest(kGoodPs));
    INFO("dxil repro: " << r.dxil.reproCmdLine);
    for (const ShaderDiag& d : r.dxil.diags)
        INFO("dxil diag: " << d.file << ":" << d.line << " " << d.message);
    for (const ShaderDiag& d : r.spirv.diags)
        INFO("spirv diag: " << d.file << ":" << d.line << " " << d.message);

    REQUIRE(r.dxil.succeeded);
    REQUIRE(r.spirv.succeeded);
    CHECK(r.AllSucceeded());
    CHECK_FALSE(r.crashed);
    CHECK_FALSE(r.fromCache);
    CHECK(HasDxbcMagic(r.dxil.bytecode));
    CHECK(HasSpirvMagic(r.spirv.bytecode));
    CHECK(r.contentHash != 0);
    CHECK(r.dxil.reproCmdLine.find("-T ps_6_5") != std::string::npos);
    CHECK(r.spirv.reproCmdLine.find("-fvk-b-shift 256 0") != std::string::npos);

    sc.Shutdown();
}

TEST_CASE("ShaderCompiler surfaces structured errors with line/col", "[shadercompile]")
{
    ShaderCompiler sc;
    REQUIRE(sc.Initialize(0.0));

    const ShaderCompileResult r = sc.CompileNow(MakeRequest(kBadPs));
    CHECK_FALSE(r.dxil.succeeded);
    CHECK_FALSE(r.spirv.succeeded);
    CHECK(r.dxil.bytecode.empty());

    REQUIRE_FALSE(r.dxil.diags.empty());
    bool foundError = false;
    for (const ShaderDiag& d : r.dxil.diags)
    {
        if (d.severity != ShaderDiagSeverity::Error)
            continue;
        foundError = true;
        CHECK(d.file == "test_material.hlsl");
        CHECK(d.line == 5);    // 1-based: kBadPs opens with a newline
        CHECK(d.col > 0);
        CHECK(d.message.find("bogus_identifier") != std::string::npos);
        break;
    }
    CHECK(foundError);

    sc.Shutdown();
}

TEST_CASE("ShaderCompiler parses warnings on successful compiles", "[shadercompile]")
{
    ShaderCompiler sc;
    REQUIRE(sc.Initialize(0.0));

    const ShaderCompileResult r = sc.CompileNow(MakeRequest(kWarnPs));
    REQUIRE(r.dxil.succeeded);

    bool foundWarning = false;
    for (const ShaderDiag& d : r.dxil.diags)
    {
        if (d.severity == ShaderDiagSeverity::Warning)
        {
            foundWarning = true;
            CHECK(d.line == 5);
            CHECK(d.message.find("truncation") != std::string::npos);
        }
    }
    CHECK(foundWarning);

    sc.Shutdown();
}

TEST_CASE("ShaderCompiler content-hash cache serves repeat compiles", "[shadercompile]")
{
    ShaderCompiler sc;
    REQUIRE(sc.Initialize(0.0));

    const ShaderCompileResult first = sc.CompileNow(MakeRequest(kGoodPs));
    REQUIRE(first.AllSucceeded());
    CHECK_FALSE(first.fromCache);

    const ShaderCompileResult second = sc.CompileNow(MakeRequest(kGoodPs));
    CHECK(second.fromCache);
    CHECK(second.contentHash == first.contentHash);
    CHECK(second.dxil.bytecode == first.dxil.bytecode);

    // Different source, different entry-conditions -> different hash.
    const ShaderCompileResult other = sc.CompileNow(MakeRequest(kWarnPs));
    CHECK(other.contentHash != first.contentHash);

    // A define changes the hash even with identical source.
    ShaderCompileRequest defined = MakeRequest(kGoodPs);
    defined.defines.emplace_back("EXTRA", "1");
    const ShaderCompileResult withDefine = sc.CompileNow(defined);
    CHECK_FALSE(withDefine.fromCache);
    CHECK(withDefine.contentHash != first.contentHash);

    sc.Shutdown();
}

TEST_CASE("ShaderCompiler async submit -> poll -> drain round-trip", "[shadercompile]")
{
    ShaderCompiler sc;
    REQUIRE(sc.Initialize(0.0));

    const std::uint64_t jobId = sc.Submit(MakeRequest(kGoodPs, /*key=*/42), /*now=*/0.0);
    REQUIRE(jobId != 0);
    CHECK_FALSE(sc.IsIdle());

    sc.Poll(/*now=*/0.0);   // debounce 0: dispatches immediately
    const auto results = DrainBlocking(sc);
    REQUIRE(results.size() == 1);
    CHECK(results[0].jobId == jobId);
    CHECK(results[0].coalesceKey == 42);
    CHECK(results[0].AllSucceeded());
    CHECK(sc.IsIdle());

    // LastGood is now populated for the key.
    const ShaderCompileResult* good = sc.LastGood(42);
    REQUIRE(good != nullptr);
    CHECK(good->contentHash == results[0].contentHash);

    sc.Shutdown();
}

TEST_CASE("ShaderCompiler last-good stays bound while a newer compile fails", "[shadercompile]")
{
    // The arc's headline failure-UX contract: a broken edit must never regress
    // the consumer below the last success for that key.
    ShaderCompiler sc;
    REQUIRE(sc.Initialize(0.0));

    sc.Submit(MakeRequest(kGoodPs, /*key=*/11), 0.0);
    sc.Poll(0.0);
    const auto good = DrainBlocking(sc);
    REQUIRE(good.size() == 1);
    REQUIRE(good[0].AllSucceeded());
    const std::uint64_t goodHash = good[0].contentHash;

    sc.Submit(MakeRequest(kBadPs, /*key=*/11), 0.0);
    sc.Poll(0.0);
    const auto bad = DrainBlocking(sc);
    REQUIRE(bad.size() == 1);
    CHECK_FALSE(bad[0].AllSucceeded());

    // The failed compile landed (diags for the panel) but LastGood still serves
    // the previous success.
    const ShaderCompileResult* lastGood = sc.LastGood(11);
    REQUIRE(lastGood != nullptr);
    CHECK(lastGood->contentHash == goodHash);
    CHECK(lastGood->AllSucceeded());

    // A synchronous cache hit on the good content refreshes LastGood too.
    const ShaderCompileResult cached = sc.CompileNow(MakeRequest(kGoodPs, /*key=*/11));
    CHECK(cached.fromCache);
    CHECK(sc.LastGood(11) != nullptr);

    sc.Shutdown();
}

TEST_CASE("ShaderCompiler debounce coalesces rapid submits to the newest", "[shadercompile]")
{
    ShaderCompiler sc;
    REQUIRE(sc.Initialize(/*debounceSeconds=*/0.5));

    sc.Submit(MakeRequest(kGoodPs, /*key=*/7), /*now=*/0.0);
    const std::uint64_t newest = sc.Submit(MakeRequest(kWarnPs, /*key=*/7), /*now=*/0.1);

    sc.Poll(/*now=*/0.3);   // inside the quiet window: nothing dispatches
    CHECK(sc.Drain().empty());
    CHECK_FALSE(sc.IsIdle());

    sc.Poll(/*now=*/0.7);   // window elapsed for the replacement
    const auto results = DrainBlocking(sc);
    REQUIRE(results.size() == 1);   // ONE compile: the older submit was replaced pre-dispatch
    CHECK(results[0].jobId == newest);

    // The compiled source is kWarnPs (the newest): its warning is present.
    bool warned = false;
    for (const ShaderDiag& d : results[0].dxil.diags)
        warned = warned || d.severity == ShaderDiagSeverity::Warning;
    CHECK(warned);

    sc.Shutdown();
}

TEST_CASE("ShaderCompiler drops results superseded after dispatch", "[shadercompile]")
{
    ShaderCompiler sc;
    REQUIRE(sc.Initialize(0.0));

    // First job dispatches and completes fully.
    sc.Submit(MakeRequest(kGoodPs, /*key=*/9), 0.0);
    sc.Poll(0.0);
    WaitUntilUndrained(sc, 1);   // result now sits undrained

    // A newer submit for the same key supersedes it before Drain runs.
    const std::uint64_t newest = sc.Submit(MakeRequest(kWarnPs, /*key=*/9), 0.0);
    sc.Poll(0.0);

    std::vector<ShaderCompileResult> all;
    for (int i = 0; i < 2000; ++i)
    {
        auto batch = sc.Drain();
        all.insert(all.end(), batch.begin(), batch.end());
        if (sc.IsIdle())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // The first (stale) result was dropped at the drain; only the newest lands.
    REQUIRE(all.size() == 1);
    CHECK(all[0].jobId == newest);

    // The superseded result's bytecode still entered the content cache --
    // undoing back to that exact source must be a cache hit, not a recompile.
    const ShaderCompileResult undone = sc.CompileNow(MakeRequest(kGoodPs));
    CHECK(undone.fromCache);

    sc.Shutdown();
}
