// `.gpudump` container (GPU crash diagnostics arc, Task 5): the raw-capture
// sibling every GPU backend writes. Pure byte shuffling -- no GPU, no device,
// no OS beyond the file write -- so it is the one part of Task 5 that CI can
// prove. Round-trips in memory and through disk, and refuses every malformed
// container shape.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/IGpuCrashBackend.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using Arcane::Diag::GpuDump;
using Arcane::Diag::GpuDumpWriter;
using Arcane::Diag::ParseGpuDump;
using Arcane::Diag::ReadGpuDump;

namespace
{
    std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "arcane_gpudump_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
        return dir;
    }

    std::string AsText(const std::vector<std::uint8_t>& bytes)
    {
        return std::string(bytes.begin(), bytes.end());
    }
}

TEST_CASE("gpudump container round-trips every section in table order", "[diag]")
{
    // The three sections a D3D12 capture produces: raw marker bytes plus the
    // backend's flattening of DRED's pointer-linked output.
    const std::uint32_t markers[4] = { 0u, 7u, 0u, 0u };

    GpuDumpWriter writer;
    writer.Add("markers", markers, sizeof(markers));
    writer.Add("dred.breadcrumb", std::string_view{ "node 0 lastCompleted=3 of 9\n" });
    writer.Add("d3d12.device", std::string_view{ "removedReason=0x887A0006\n" });

    REQUIRE(writer.SectionCount() == 3);
    CHECK(writer.Inventory() == "markers, dred.breadcrumb, d3d12.device");

    const auto dump = ParseGpuDump(writer.Build());
    REQUIRE(dump.has_value());
    CHECK(dump->version == Arcane::Diag::kGpuDumpVersion);
    REQUIRE(dump->sections.size() == 3);

    CHECK(dump->sections[0].tag == "markers");
    REQUIRE(dump->sections[0].bytes.size() == sizeof(markers));
    CHECK(std::memcmp(dump->sections[0].bytes.data(), markers, sizeof(markers)) == 0);

    CHECK(dump->sections[1].tag == "dred.breadcrumb");
    CHECK(AsText(dump->sections[1].bytes) == "node 0 lastCompleted=3 of 9\n");

    CHECK(dump->sections[2].tag == "d3d12.device");
    CHECK(AsText(dump->sections[2].bytes) == "removedReason=0x887A0006\n");
}

TEST_CASE("gpudump keeps empty sections -- the table IS the capture inventory", "[diag]")
{
    // "DRED was queried and had nothing" must stay distinguishable from
    // "DRED was never queried", which is exactly the empty-section case.
    GpuDumpWriter writer;
    writer.Add("markers", std::string_view{ "ab" });
    writer.Add("dred.pagefault", nullptr, 0);

    const auto dump = ParseGpuDump(writer.Build());
    REQUIRE(dump.has_value());
    REQUIRE(dump->sections.size() == 2);
    CHECK(dump->sections[1].tag == "dred.pagefault");
    CHECK(dump->sections[1].bytes.empty());
}

TEST_CASE("gpudump with no sections is still a valid container", "[diag]")
{
    const auto dump = ParseGpuDump(GpuDumpWriter{}.Build());
    REQUIRE(dump.has_value());
    CHECK(dump->sections.empty());
}

TEST_CASE("gpudump truncates an over-long tag to the 16-byte table field", "[diag]")
{
    GpuDumpWriter writer;
    writer.Add("a-very-long-section-tag-indeed", std::string_view{ "x" });

    const auto dump = ParseGpuDump(writer.Build());
    REQUIRE(dump.has_value());
    REQUIRE(dump->sections.size() == 1);
    CHECK(dump->sections[0].tag == "a-very-long-sect");
    CHECK(dump->sections[0].tag.size() == Arcane::Diag::kGpuDumpTagBytes);
}

TEST_CASE("gpudump round-trips through disk", "[diag]")
{
    const std::filesystem::path path = TempDir("disk") / "report.gpudump";

    const std::uint32_t markers[2] = { 5u, 0u };
    GpuDumpWriter writer;
    writer.Add("markers", markers, sizeof(markers));
    writer.Add("dred.breadcrumb", std::string_view{ "node 0\n" });
    REQUIRE(writer.Write(path));
    REQUIRE(std::filesystem::exists(path));

    const auto dump = ReadGpuDump(path);
    REQUIRE(dump.has_value());
    REQUIRE(dump->sections.size() == 2);
    CHECK(dump->sections[0].tag == "markers");
    REQUIRE(dump->sections[0].bytes.size() == sizeof(markers));
    CHECK(std::memcmp(dump->sections[0].bytes.data(), markers, sizeof(markers)) == 0);
    CHECK(AsText(dump->sections[1].bytes) == "node 0\n");
}

TEST_CASE("gpudump parse rejects a bad magic, a bad version, and a truncated file", "[diag]")
{
    GpuDumpWriter writer;
    writer.Add("markers", std::string_view{ "0123456789" });
    const std::string good = writer.Build();

    CHECK_FALSE(ParseGpuDump("").has_value());
    CHECK_FALSE(ParseGpuDump("AGP").has_value());

    std::string badMagic = good;
    badMagic[0] = 'X';
    CHECK_FALSE(ParseGpuDump(badMagic).has_value());

    std::string badVersion = good;
    badVersion[4] = static_cast<char>(Arcane::Diag::kGpuDumpVersion + 1);
    CHECK_FALSE(ParseGpuDump(badVersion).has_value());

    // Payload clipped: the surviving table entry claims bytes past the end.
    std::string clipped = good;
    clipped.resize(clipped.size() - 1);
    CHECK_FALSE(ParseGpuDump(clipped).has_value());

    // Table clipped: the header claims a section whose entry is not there.
    std::string noTable = good.substr(0, Arcane::Diag::kGpuDumpHeaderBytes + 4);
    CHECK_FALSE(ParseGpuDump(noTable).has_value());

    CHECK(ParseGpuDump(good).has_value());
}

TEST_CASE("gpudump ReadGpuDump fails cleanly on a missing file", "[diag]")
{
    CHECK_FALSE(ReadGpuDump(TempDir("missing") / "nope.gpudump").has_value());
}
