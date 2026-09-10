// F2c Task 2: the glTF fixture corpus's ARRIVAL GATE. It asserts the files exist,
// are staged beside the exe, and carry the structural marks each later task depends
// on -- deliberately WITHOUT parsing them through cgltf, so a corpus regression is
// distinguishable from an importer regression. The corpus's own source of truth is
// scripts/make-mesh-fixtures.ps1; re-running it must leave the tree clean, which is
// the property Step 5 checks at authoring time and this file cannot.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // Tests run FROM the exe dir, so the staged corpus is exe-relative.
    fs::path FixtureDir() { return fs::path("data") / "gltf"; }

    std::vector<std::uint8_t> ReadAll(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    }
}

TEST_CASE("gltf corpus: every fixture spec s9 names is staged beside the exe",
          "[fixture][gltf]")
{
    for (const char* name : { "single.glb", "multi.glb", "nested.gltf", "nested.bin",
                              "mirrored.glb", "embedded_tex.glb", "degenerate.glb",
                              "empty.glb", "bad_sparse.glb", "requires_draco.gltf" })
    {
        INFO(name);
        CHECK(fs::exists(FixtureDir() / name));
    }
}

TEST_CASE("gltf corpus: every .glb carries a well-formed GLB container header",
          "[fixture][gltf]")
{
    for (const char* name : { "single.glb", "multi.glb", "mirrored.glb",
                              "embedded_tex.glb", "degenerate.glb", "empty.glb",
                              "bad_sparse.glb" })
    {
        INFO(name);
        const std::vector<std::uint8_t> bytes = ReadAll(FixtureDir() / name);
        REQUIRE(bytes.size() >= 20);
        // magic "glTF", version 2, then a total length that matches the file.
        CHECK(bytes[0] == 'g'); CHECK(bytes[1] == 'l');
        CHECK(bytes[2] == 'T'); CHECK(bytes[3] == 'F');
        const auto u32 = [&](std::size_t at) {
            return static_cast<std::uint32_t>(bytes[at])
                 | (static_cast<std::uint32_t>(bytes[at + 1]) << 8)
                 | (static_cast<std::uint32_t>(bytes[at + 2]) << 16)
                 | (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
        };
        CHECK(u32(4) == 2u);
        CHECK(u32(8) == static_cast<std::uint32_t>(bytes.size()));
    }
}

TEST_CASE("gltf corpus: the structural marks each later task depends on",
          "[fixture][gltf]")
{
    const auto text = [](const fs::path& p) {
        const std::vector<std::uint8_t> b = ReadAll(p);
        return std::string(b.begin(), b.end());
    };

    // A1's dedup pin: TWO primitives name the SAME material, a third names another.
    const std::string multi = text(FixtureDir() / "multi.glb");
    CHECK(multi.find("Metal") != std::string::npos);
    CHECK(multi.find("Paint") != std::string::npos);

    // The external-buffer pair (s5.4): the .gltf references the .bin BY URI, so
    // editing the .bin must move the cook key.
    const std::string nested = text(FixtureDir() / "nested.gltf");
    CHECK(nested.find("nested.bin") != std::string::npos);

    // A2 part 1: the refusal rule reads extensionsRequired, so the fixture must
    // actually carry one.
    const std::string draco = text(FixtureDir() / "requires_draco.gltf");
    CHECK(draco.find("extensionsRequired") != std::string::npos);
    CHECK(draco.find("KHR_draco_mesh_compression") != std::string::npos);

    // s4.3's winding-flip fixture needs a genuinely negative determinant.
    const std::string mirrored = text(FixtureDir() / "mirrored.glb");
    CHECK(mirrored.find("-1") != std::string::npos);
}
