// F2c Task 13: embedded-texture extraction at discovery (spec s5.5, A4). A .glb's
// embedded images become loose .png siblings beside the source -- which the ordinary
// texture-cook path then picks up on its own next sweep -- never overwriting an
// existing file. This TU drives MeshImportWave.hpp's PURE halves directly:
// UniqueSiblingPath's collision-suffix rule, ImageFileStem's name-or-fallback +
// sanitisation, and ExtractEmbeddedTextures' survey -> compute destination -> skip
// or write pipeline. [editor] -- CPU-only, no GPU/ImGui/Project/device involved
// (MeshImportWave.hpp's own header comment states the "PURE by design" rule).

#include <catch2/catch_test_macros.hpp>

#include <Project/MeshImportWave.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using namespace Arcane::Editor;

namespace
{
    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_mesh_import_wave_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    void Touch(const fs::path& p)
    {
        std::ofstream(p, std::ios::binary) << "x";
    }

    void WriteBytes(const fs::path& p, const std::vector<std::uint8_t>& bytes)
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        if (!bytes.empty())
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
    }

    std::vector<std::uint8_t> ReadBytes(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        REQUIRE(in.good());
        in.seekg(0, std::ios::end);
        const auto len = static_cast<std::size_t>(in.tellg());
        in.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> out(len);
        if (len > 0)
            in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
        REQUIRE(in.good());
        return out;
    }
}

TEST_CASE("import wave: UniqueSiblingPath never clobbers an existing file", "[editor]")
{
    const fs::path dir = TempDir("unique_sibling");
    CHECK(UniqueSiblingPath(dir, "albedo", ".png") == dir / "albedo.png");
    Touch(dir / "albedo.png");
    CHECK(UniqueSiblingPath(dir, "albedo", ".png") == dir / "albedo-1.png");
    Touch(dir / "albedo-1.png");
    CHECK(UniqueSiblingPath(dir, "albedo", ".png") == dir / "albedo-2.png");
}

TEST_CASE("import wave: an unnamed glTF image falls back to a source-derived name",
          "[editor]")
{
    CHECK(ImageFileStem("albedo", "prop", 0) == "albedo");
    CHECK(ImageFileStem("", "prop", 2) == "prop-2");
    // Arbitrary UTF-8 is sanitised: a separator in a glTF name must never steer the
    // write out of the folder it was meant for.
    CHECK(ImageFileStem("../../evil", "prop", 0).find('/') == std::string::npos);
    CHECK(ImageFileStem("../../evil", "prop", 0).find('\\') == std::string::npos);
}

TEST_CASE("import wave: an embedded texture extracts ONCE and is never overwritten"
          " (A4)", "[editor]")
{
    // s5.5 as amended: extraction happens only when the destination is ABSENT. A
    // user's edited or replaced .png survives every re-import -- the same
    // import-never-overwrites invariant s6 states for materials, which A4 exists to
    // extend to the equally user-visible extracted image.
    const fs::path dir = TempDir("extract_once");
    fs::copy_file(fs::path("data") / "gltf" / "embedded_tex.glb", dir / "embedded_tex.glb");

    const auto first = ExtractEmbeddedTextures(dir / "embedded_tex.glb");
    REQUIRE(first.size() == 1u);
    CHECK(fs::exists(first[0]));

    // The user edits the extracted file.
    WriteBytes(first[0], std::vector<std::uint8_t>{ 0xDE, 0xAD, 0xBE, 0xEF });

    const auto second = ExtractEmbeddedTextures(dir / "embedded_tex.glb");
    CHECK(second.empty());                                   // nothing re-extracted
    CHECK(ReadBytes(first[0]) == std::vector<std::uint8_t>{ 0xDE, 0xAD, 0xBE, 0xEF });
}

TEST_CASE("import wave: two glb files embedding the same image name do not collide",
          "[editor]")
{
    // Both .glb files embed an image named "albedo" (embedded_tex.glb's fixture,
    // copied verbatim under two different source names). ImageFileStem prefers the
    // glTF-authored name over the source filename (its own header comment), so both
    // sources compute the SAME natural destination -- "albedo.png". A4's no-overwrite
    // rule (proven above for a hand-edited file) applies identically to a second
    // SOURCE's collision at that name: the first extraction wins the name, and the
    // second is skipped rather than overwriting it -- never a corrupted or
    // silently-replaced file, whichever source got there first.
    const fs::path dir = TempDir("two_sources_same_name");
    fs::copy_file(fs::path("data") / "gltf" / "embedded_tex.glb", dir / "propA.glb");
    fs::copy_file(fs::path("data") / "gltf" / "embedded_tex.glb", dir / "propB.glb");

    const auto fromA = ExtractEmbeddedTextures(dir / "propA.glb");
    REQUIRE(fromA.size() == 1u);
    CHECK(fromA[0] == dir / "albedo.png");
    const std::vector<std::uint8_t> originalBytes = ReadBytes(fromA[0]);

    const auto fromB = ExtractEmbeddedTextures(dir / "propB.glb");
    CHECK(fromB.empty());                       // "albedo.png" already taken -- skipped
    CHECK(ReadBytes(dir / "albedo.png") == originalBytes);   // untouched by the second source

    // Exactly one .png landed in the folder -- no "-1" clone of the same bytes under
    // a second name.
    std::size_t pngCount = 0;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().extension() == ".png")
            ++pngCount;
    CHECK(pngCount == 1u);
}

TEST_CASE("import wave: a glb with no embedded images extracts nothing, quietly",
          "[editor]")
{
    const fs::path dir = TempDir("no_embedded_images");
    fs::copy_file(fs::path("data") / "gltf" / "single.glb", dir / "single.glb");

    const auto extracted = ExtractEmbeddedTextures(dir / "single.glb");
    CHECK(extracted.empty());

    std::size_t pngCount = 0;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().extension() == ".png")
            ++pngCount;
    CHECK(pngCount == 0u);
}
