// .arcdiag envelope (GPU crash diagnostics arc, Task 2): round-trip every
// field through Serialize/Parse and WriteFile/ReadFile, and confirm Parse
// refuses a missing/invalid formatVersion or a nil guid. Pure unit -- no
// GPU/OS dependency.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Guid.hpp>

#include <filesystem>
#include <fstream>

using namespace Arcane;

namespace
{
    std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path d =
            std::filesystem::temp_directory_path() / "arcane_diag_envelope_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }
}

TEST_CASE("arcdiag envelope round-trips with its guid", "[diag]")
{
    Arcane::Diag::Envelope e;
    e.guid = Arcane::Guid::Generate();
    e.kind = "gpu-crash"; e.phase = "editor-frame"; e.appName = "DiagTest";
    e.queues.push_back({ "direct", "pass:tonemap", { "pass:imgui" } });
    e.fault = { "page-fault", "0xDEADBEEF0000", "ParticleIndices" };
    e.siblingTxt = "r.txt"; e.siblingDmp = "r.dmp";
    e.activeLayers = { "breadcrumbs:pass", "dred:full" };

    const auto back = Arcane::Diag::Parse(Arcane::Diag::Serialize(e));
    REQUIRE(back.has_value());
    CHECK(back->guid == e.guid);
    CHECK(back->kind == "gpu-crash");
    REQUIRE(back->queues.size() == 1);
    CHECK(back->queues[0].lastCompleted == "pass:tonemap");
    CHECK(back->fault.resource == "ParticleIndices");
    CHECK(back->activeLayers.size() == 2);
}

TEST_CASE("arcdiag parse rejects nil guid and bad version", "[diag]")
{
    CHECK_FALSE(Arcane::Diag::Parse("{}").has_value());
    CHECK_FALSE(Arcane::Diag::Parse("{\"formatVersion\":1,\"guid\":\"00000000-0000-0000-0000-000000000000\"}").has_value());
}

TEST_CASE("arcdiag envelope round-trips every field, including all three sibling strings", "[diag]")
{
    Arcane::Diag::Envelope e;
    e.formatVersion = 1;
    e.guid = Arcane::Guid::Generate();
    e.kind = "gpu-crash";
    e.timestampUtc = "2026-08-11T12:34:56Z";
    e.appName = "ArcaneEditor";
    e.phase = "editor frame loop";
    e.buildInfo = "Debug@8c728637";
    e.cpuThreadSummary = "Main thread: RenderSceneToViewport\nWorker 1: idle\n";
    e.queues.push_back({ "direct", "pass:tonemap", { "pass:imgui", "pass:outline" } });
    e.queues.push_back({ "compute", "", {} });
    e.fault = { "page-fault", "0xDEADBEEF0000", "ParticleIndices" };
    e.siblingTxt = "r.txt";
    e.siblingDmp = "r.dmp";
    e.siblingGpuDump = "r.gpudump";
    e.activeLayers = { "breadcrumbs:pass", "dred:full", "markers:buffer" };

    const auto back = Arcane::Diag::Parse(Arcane::Diag::Serialize(e));
    REQUIRE(back.has_value());
    CHECK(back->formatVersion == e.formatVersion);
    CHECK(back->guid == e.guid);
    CHECK(back->kind == e.kind);
    CHECK(back->timestampUtc == e.timestampUtc);
    CHECK(back->appName == e.appName);
    CHECK(back->phase == e.phase);
    CHECK(back->buildInfo == e.buildInfo);
    CHECK(back->cpuThreadSummary == e.cpuThreadSummary);
    REQUIRE(back->queues.size() == 2);
    CHECK(back->queues[0].name == "direct");
    CHECK(back->queues[0].lastCompleted == "pass:tonemap");
    REQUIRE(back->queues[0].inFlight.size() == 2);
    CHECK(back->queues[0].inFlight[0] == "pass:imgui");
    CHECK(back->queues[0].inFlight[1] == "pass:outline");
    CHECK(back->queues[1].name == "compute");
    CHECK(back->queues[1].lastCompleted.empty());
    CHECK(back->queues[1].inFlight.empty());
    CHECK(back->fault.type == "page-fault");
    CHECK(back->fault.address == "0xDEADBEEF0000");
    CHECK(back->fault.resource == "ParticleIndices");
    CHECK(back->siblingTxt == "r.txt");
    CHECK(back->siblingDmp == "r.dmp");
    CHECK(back->siblingGpuDump == "r.gpudump");
    REQUIRE(back->activeLayers.size() == 3);
    CHECK(back->activeLayers[2] == "markers:buffer");
}

TEST_CASE("arcdiag parse ignores unknown extra keys (forward compat)", "[diag]")
{
    const std::string json =
        "{\"formatVersion\":1,"
        "\"guid\":\"" + Arcane::Guid::Generate().ToString() + "\","
        "\"kind\":\"gpu-crash\","
        "\"futureField\":\"from-a-later-format-version\","
        "\"queues\":[{\"name\":\"direct\",\"lastCompleted\":\"pass:x\",\"inFlight\":[],"
        "\"futureQueueField\":42}]}";

    const auto back = Arcane::Diag::Parse(json);
    REQUIRE(back.has_value());
    CHECK(back->kind == "gpu-crash");
    REQUIRE(back->queues.size() == 1);
    CHECK(back->queues[0].name == "direct");
}

TEST_CASE("arcdiag parse rejects a wrong-typed formatVersion", "[diag]")
{
    const std::string json =
        "{\"formatVersion\":\"1\","
        "\"guid\":\"" + Arcane::Guid::Generate().ToString() + "\"}";
    CHECK_FALSE(Arcane::Diag::Parse(json).has_value());
}

TEST_CASE("arcdiag WriteFile/ReadFile round-trips through disk as UTF-8 with no BOM", "[diag]")
{
    const auto dir = TempDir("roundtrip");
    const auto file = dir / "crash-20260811-120000-pid1234.arcdiag";

    Arcane::Diag::Envelope e;
    e.guid = Arcane::Guid::Generate();
    e.kind = "gpu-crash";
    e.appName = "ArcaneEditor";
    e.fault = { "hang", "", "" };
    e.activeLayers = { "breadcrumbs:pass" };

    REQUIRE(Arcane::Diag::WriteFile(e, file));

    // No UTF-8 BOM (EF BB BF) at the start of the file.
    std::ifstream raw(file, std::ios::binary);
    REQUIRE(raw.good());
    unsigned char bytes[3] = { 0, 0, 0 };
    raw.read(reinterpret_cast<char*>(bytes), 3);
    CHECK_FALSE((bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF));

    const auto back = Arcane::Diag::ReadFile(file);
    REQUIRE(back.has_value());
    CHECK(back->guid == e.guid);
    CHECK(back->kind == "gpu-crash");
    CHECK(back->fault.type == "hang");
}

TEST_CASE("arcdiag ReadFile fails cleanly on a missing file", "[diag]")
{
    const auto dir = TempDir("missing");
    CHECK_FALSE(Arcane::Diag::ReadFile(dir / "does-not-exist.arcdiag").has_value());
}
