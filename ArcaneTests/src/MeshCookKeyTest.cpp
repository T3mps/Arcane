// F2c Task 5: MeshMetaSettings (the ".meta" "mesh" block, v1 has NO user-facing knobs -- spec
// s5.4) and ComputeMeshCookKey, the mesh half of the F2b triple
// (hash(source bytes + import settings + importer version), CookKey.hpp:3-10) WIDENED to also
// cover every external buffer a .gltf references (spec s5.4's load-bearing addition: a .gltf's
// own bytes never change when a referenced "geometry.bin" is re-exported, so a key that ignored
// buffers would serve a stale artifact forever).
//
// Pins: determinism + every term moving the key (source, settings, importer version -- the same
// four-way shape TextureImporterTest's own cook-key case already established for the texture
// half); the external-buffer case s5.4 exists for; the length-prefix's buffer-boundary
// non-fungibility (two 8-byte buffers must not collide with one 16-byte buffer of the same
// concatenated bytes); the source/buffer-list boundary's own non-fungibility (a leading u32
// source-length prefix is what keeps a source that CONTAINS a buffer-shaped byte sequence from
// colliding with an actual empty-source-plus-that-buffer encoding); and
// MeshMetaSettings::FromMetaJson's tolerant fallback (missing block, wrong-typed field,
// hand-edited negative, and its own round-trip through ToMetaJson). Tagged "[pipeline]", the
// same tag this suite's sibling AssetPipeline*Test.cpp files use.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/AssetPipeline/CookKey.hpp>
#include <Arcane/AssetPipeline/MeshMetaSettings.hpp>

#include <Json.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using Arcane::AssetPipeline::ComputeMeshCookKey;
using Arcane::AssetPipeline::kMeshImporterVersion;
using Arcane::AssetPipeline::MeshMetaSettings;

namespace
{
    // Same generator every AssetPipeline*Test.cpp file duplicates locally rather than sharing
    // (no test-only helper library crosses the same ArcaneClient/ArcaneAssetPipeline boundary
    // the production code itself refuses to cross).
    std::vector<std::byte> PatternBytes(std::size_t n, std::uint8_t seed)
    {
        std::vector<std::byte> out(n);
        for (std::size_t i = 0; i < n; ++i)
            out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + i * 7));
        return out;
    }
}

TEST_CASE("mesh cook key: deterministic, and every term moves it", "[pipeline]")
{
    const std::vector<std::byte> src  = PatternBytes(32, 0x10);
    const std::vector<std::byte> buf0 = PatternBytes(16, 0x20);
    const std::span<const std::byte> bufs0[] = { buf0 };
    const MeshMetaSettings settings{};

    const std::uint64_t base = ComputeMeshCookKey(src, bufs0, settings, kMeshImporterVersion);
    CHECK(base == ComputeMeshCookKey(src, bufs0, settings, kMeshImporterVersion));

    const std::vector<std::byte> src2 = PatternBytes(32, 0x11);
    CHECK(ComputeMeshCookKey(src2, bufs0, settings, kMeshImporterVersion) != base);

    MeshMetaSettings bumped; bumped.settingsVersion = 2;
    CHECK(ComputeMeshCookKey(src, bufs0, bumped, kMeshImporterVersion) != base);

    CHECK(ComputeMeshCookKey(src, bufs0, settings, kMeshImporterVersion + 1) != base);
}

TEST_CASE("mesh cook key: editing an external .bin moves the key (spec s5.4)",
          "[pipeline]")
{
    // THE CASE s5.4 EXISTS FOR. The .gltf's own bytes are byte-identical across both
    // calls; only the referenced buffer changed. A key that ignored buffers would be
    // EQUAL here, and a re-exported .bin would never recook.
    const std::vector<std::byte> gltf = PatternBytes(48, 0x30);
    const std::vector<std::byte> binA = PatternBytes(24, 0x40);
    const std::vector<std::byte> binB = PatternBytes(24, 0x41);
    const std::span<const std::byte> a[] = { binA };
    const std::span<const std::byte> b[] = { binB };
    CHECK(ComputeMeshCookKey(gltf, a, MeshMetaSettings{}, kMeshImporterVersion)
          != ComputeMeshCookKey(gltf, b, MeshMetaSettings{}, kMeshImporterVersion));
}

TEST_CASE("mesh cook key: buffer boundaries are not fungible", "[pipeline]")
{
    // Two 8-byte buffers must not hash the same as one 16-byte buffer with the same
    // contents -- the u32 length prefix is what guarantees it. Without the prefix, a
    // re-export that merged two buffers into one would keep the OLD key and serve a
    // stale artifact.
    const std::vector<std::byte> src = PatternBytes(8, 0x50);
    const std::vector<std::byte> lo(8, std::byte{ 0xAA });
    const std::vector<std::byte> hi(8, std::byte{ 0xAA });
    std::vector<std::byte> joined(lo);
    joined.insert(joined.end(), hi.begin(), hi.end());
    const std::span<const std::byte> two[] = { lo, hi };
    const std::span<const std::byte> one[] = { joined };
    CHECK(ComputeMeshCookKey(src, two, MeshMetaSettings{}, kMeshImporterVersion)
          != ComputeMeshCookKey(src, one, MeshMetaSettings{}, kMeshImporterVersion));
}

TEST_CASE("mesh cook key: the source/buffer-list boundary is not fungible", "[pipeline]")
{
    // Without a leading length prefix on sourceBytes, the flat byte stream the hasher
    // consumes does not distinguish where the source region ends and the buffer-list
    // region begins: a 9-byte source that happens to CONTAIN a u32-length-prefixed 5-byte
    // "buffer" (literally the bytes "05 00 00 00 BB BB BB BB BB") with zero real external
    // buffers is byte-identical to an EMPTY source followed by one genuine 5-byte buffer
    // {BB BB BB BB BB} -- both feed "05 00 00 00 BB BB BB BB BB" ahead of the fixed
    // settings/importer-version tail. The leading source-length prefix is what pins the
    // boundary and makes the two cases distinguishable.
    const std::vector<std::byte> nineByteSrc = { std::byte{ 0x05 }, std::byte{ 0x00 }, std::byte{ 0x00 },
                                                   std::byte{ 0x00 }, std::byte{ 0xBB }, std::byte{ 0xBB },
                                                   std::byte{ 0xBB }, std::byte{ 0xBB }, std::byte{ 0xBB } };
    const std::vector<std::byte> emptySrc{};
    const std::vector<std::byte> bbBuffer(5, std::byte{ 0xBB });
    const std::span<const std::span<const std::byte>> noBuffers{};
    const std::span<const std::byte> oneBuffer[] = { bbBuffer };
    CHECK(ComputeMeshCookKey(nineByteSrc, noBuffers, MeshMetaSettings{}, kMeshImporterVersion)
          != ComputeMeshCookKey(emptySrc, oneBuffer, MeshMetaSettings{}, kMeshImporterVersion));
}

TEST_CASE("mesh meta settings: a missing or malformed block falls back, never throws",
          "[pipeline]")
{
    CHECK(MeshMetaSettings::FromMetaJson(nlohmann::json::object()).settingsVersion == 1u);
    CHECK(MeshMetaSettings::FromMetaJson(nlohmann::json{ { "settingsVersion", "two" } })
              .settingsVersion == 1u);
    // is_number_unsigned, not is_number -- a hand-edited -3 must fall back, not wrap
    // to 4294967293 the way get<uint32_t>() would (LoadMeshAsset's own readUint rule).
    CHECK(MeshMetaSettings::FromMetaJson(nlohmann::json{ { "settingsVersion", -3 } })
              .settingsVersion == 1u);
    CHECK(MeshMetaSettings::FromMetaJson(MeshMetaSettings{}.ToMetaJson())
              .settingsVersion == 1u);
}
