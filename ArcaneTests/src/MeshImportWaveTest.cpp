// F2c Task 13: embedded-texture extraction at discovery (spec s5.5, A4). A .glb's
// embedded images become loose .png siblings beside the source -- which the ordinary
// texture-cook path then picks up on its own next sweep -- via a three-way rule that
// reconciles s5.5's never-overwrite invariant with A4's suffix-on-collision one:
// nothing at the natural name -> write; identical bytes there -> skip (already
// extracted); different bytes there -> never overwrite, write to a suffixed sibling
// instead (MeshImportWave.hpp's ExtractEmbeddedTextures comment has the full
// account). This TU drives MeshImportWave.hpp's PURE halves directly:
// UniqueSiblingPath's collision-suffix rule, ImageFileStem's name-or-fallback +
// sanitisation, and ExtractEmbeddedTextures' three arms. [editor] -- CPU-only, no
// GPU/ImGui/Project/device involved (MeshImportWave.hpp's own header comment states
// the "PURE by design" rule).

#include <catch2/catch_test_macros.hpp>

#include <Project/MeshImportWave.hpp>

#include <algorithm>
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
    // The three-way rule (MeshImportWave.hpp's ExtractEmbeddedTextures comment):
    // nothing there -> write; identical bytes there -> skip; DIFFERENT bytes there
    // -> never overwrite, write the fresh bytes to a suffixed sibling instead. A
    // user's edit makes the natural destination's bytes diverge from what the source
    // would extract, so re-discovery hits arm 3, not a skip: the edit survives
    // completely untouched, AND the source's own (still-current) image still gets a
    // file on disk -- just not at the name the edit now owns.
    const fs::path dir = TempDir("extract_once");
    fs::copy_file(fs::path("data") / "gltf" / "embedded_tex.glb", dir / "embedded_tex.glb");

    const auto first = ExtractEmbeddedTextures(dir / "embedded_tex.glb");
    REQUIRE(first.size() == 1u);
    CHECK(fs::exists(first[0]));
    const std::vector<std::uint8_t> originalBytes = ReadBytes(first[0]);

    // The user edits the extracted file.
    WriteBytes(first[0], std::vector<std::uint8_t>{ 0xDE, 0xAD, 0xBE, 0xEF });

    const auto second = ExtractEmbeddedTextures(dir / "embedded_tex.glb");

    // (a) the edit is never overwritten.
    CHECK(ReadBytes(first[0]) == std::vector<std::uint8_t>{ 0xDE, 0xAD, 0xBE, 0xEF });
    // (b) re-discovery lands the original bytes at a suffixed sibling, not at
    //     first[0].
    REQUIRE(second.size() == 1u);
    CHECK(second[0] != first[0]);
    CHECK(second[0] == dir / "albedo-1.png");
    CHECK(ReadBytes(second[0]) == originalBytes);
}

TEST_CASE("import wave: two glb files embedding a same-named but DIFFERENT image do"
          " not collide", "[editor]")
{
    // Both .glb files start as byte-for-byte copies of embedded_tex.glb, so both
    // embed an image named "albedo" -- ImageFileStem prefers the glTF-authored name
    // over the source filename (its own header comment), so both sources compute the
    // SAME natural destination, "albedo.png". The SECOND copy's embedded PNG payload
    // is then corrupted by one byte (found by searching the raw .glb file for the
    // original PNG's own byte sequence, so this does not depend on the fixture
    // generator's internal layout; the flipped byte sits well past the 8-byte PNG
    // signature -- extraction never decodes the payload, so a structurally-invalid
    // PNG is still perfectly valid bytes to copy verbatim). A genuinely different
    // image under the same name is arm 3 of the three-way rule: a real collision,
    // never overwriting the first file, landing at UniqueSiblingPath's next free
    // name instead.
    const fs::path dir = TempDir("two_sources_diff_bytes");
    fs::copy_file(fs::path("data") / "gltf" / "embedded_tex.glb", dir / "propA.glb");
    fs::copy_file(fs::path("data") / "gltf" / "embedded_tex.glb", dir / "propB.glb");

    const auto fromA = ExtractEmbeddedTextures(dir / "propA.glb");
    REQUIRE(fromA.size() == 1u);
    CHECK(fromA[0] == dir / "albedo.png");
    const std::vector<std::uint8_t> originalPngBytes = ReadBytes(fromA[0]);
    REQUIRE(originalPngBytes.size() > 8u);   // more than just the PNG signature

    // Corrupt propB.glb's own embedded copy of that same PNG payload, in place.
    std::vector<std::uint8_t> rawB = ReadBytes(dir / "propB.glb");
    const auto found = std::search(rawB.begin(), rawB.end(),
                                    originalPngBytes.begin(), originalPngBytes.end());
    REQUIRE(found != rawB.end());
    const auto payloadOffset = static_cast<std::size_t>(found - rawB.begin());
    rawB[payloadOffset + 8] ^= 0xFFu;   // one byte, past the 8-byte PNG signature
    WriteBytes(dir / "propB.glb", rawB);

    const auto fromB = ExtractEmbeddedTextures(dir / "propB.glb");
    REQUIRE(fromB.size() == 1u);
    CHECK(fromB[0] != fromA[0]);
    CHECK(fs::exists(fromB[0]));
    CHECK(ReadBytes(fromB[0]) != originalPngBytes);      // the CORRUPTED bytes landed here
    CHECK(ReadBytes(fromA[0]) == originalPngBytes);      // the first file is untouched
}

TEST_CASE("import wave: two glb files embedding byte-identical images dedupe"
          " silently", "[editor]")
{
    // Companion to the collision case above, pinning the three-way rule's middle
    // arm: when a second source's embedded image is BYTE-IDENTICAL to what already
    // sits at the natural destination (two meshes genuinely sharing one texture, or
    // the same source re-discovered with nothing changed), nothing new is written --
    // no needless "-1" clone of content that is already there.
    const fs::path dir = TempDir("two_sources_same_bytes");
    fs::copy_file(fs::path("data") / "gltf" / "embedded_tex.glb", dir / "propC.glb");
    fs::copy_file(fs::path("data") / "gltf" / "embedded_tex.glb", dir / "propD.glb");

    const auto fromC = ExtractEmbeddedTextures(dir / "propC.glb");
    REQUIRE(fromC.size() == 1u);

    const auto fromD = ExtractEmbeddedTextures(dir / "propD.glb");
    CHECK(fromD.empty());

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
