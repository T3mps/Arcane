// 3D-readiness Plan A, Task 3: the v1 .arcart fixtures. A texture and a mesh
// artifact are written from fixed guids (never Guid::Generate) and checked in
// under Tests/ArcaneTests/data/artifacts/. Plan B compares a fresh write of the
// moved codec against these bytes. The capture case is hidden ([.]) and armed
// by the same environment switch as the physics baselines.
#include <catch2/catch_test_macros.hpp>

#include "Helpers/ConstantScan.hpp"
#include "Helpers/PhysicsV1Baseline.hpp"   // ReadFileBinary / WriteFileBinary

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/TextureImporter.hpp>
#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>
#include <Arcane/Crypto/Crypto.hpp>
#include <Arcane/Guid.hpp>

#include <stb_image_write.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace Arcane::AssetPipeline;
using namespace Arcane::Test;

namespace
{
    constexpr Arcane::Guid kV1TextureGuid{ 0x1111111122224333ull, 0x8444555555555555ull };
    constexpr Arcane::Guid kV1MeshGuid{ 0xAAAAAAAABBBBCCCCull, 0xDDDDEEEEFFFF0000ull };

    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_artifact_v1_fixture_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    // ---- copied byte-for-byte from ArtifactReaderTest.cpp (file-local there) -------------

    void CollectPngBytes(void* ctx, void* data, int size)
    {
        auto* out = static_cast<std::vector<std::byte>*>(ctx);
        const auto* p = static_cast<std::byte*>(data);
        out->insert(out->end(), p, p + size);
    }

    std::vector<std::byte> EncodePng(int width, int height, const std::vector<unsigned char>& rgba)
    {
        std::vector<std::byte> out;
        REQUIRE(stbi_write_png_to_func(&CollectPngBytes, &out, width, height, 4, rgba.data(), width * 4) != 0);
        return out;
    }

    std::vector<unsigned char> GradientPixels(int width, int height)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(width) * height * 4);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                unsigned char* p = px.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                p[0] = static_cast<unsigned char>((x * 255) / (width > 1 ? width - 1 : 1));
                p[1] = static_cast<unsigned char>((y * 255) / (height > 1 ? height - 1 : 1));
                p[2] = 128;
                p[3] = 255;
            }
        }
        return px;
    }

    TextureMetaSettings DefaultRgba8Settings()
    {
        TextureMetaSettings settings;
        settings.format = TextureMetaSettings::Format::Rgba8;
        return settings;
    }

    // ---- the fixtures ---------------------------------------------------------------------

    std::string WriteV1TextureToTemp()
    {
        const fs::path path = TempDir("texture") / "v1-texture.arcart";
        const auto png = EncodePng(17, 9, GradientPixels(17, 9));
        auto imported = ImportTexture(png, kV1TextureGuid, DefaultRgba8Settings());
        REQUIRE(imported.has_value());
        REQUIRE(WriteTextureArtifact(path, imported->desc, imported->payload, imported->thumbRgba));
        return ReadFileBinary(path);
    }

    std::string WriteV1MeshToTemp()
    {
        const fs::path path = TempDir("mesh") / "v1-mesh.arcart";
        MeshArtifactDesc desc{};
        desc.contentKind = ContentKind::Mesh;
        desc.sourceGuid = kV1MeshGuid;
        desc.sourceHash = 0x0123456789ABCDEFULL;
        desc.importerVersion = 3;
        desc.vertexCount = 4;
        desc.indexCount = 9;
        desc.sectionCount = 2;
        desc.indexWidth = 4;
        desc.aabbMin[0] = -1.5f; desc.aabbMin[1] = -2.25f; desc.aabbMin[2] = -3.125f;
        desc.aabbMax[0] =  4.5f; desc.aabbMax[1] =  5.25f; desc.aabbMax[2] =  6.125f;
        desc.sections = { { "Metal", 0, 6, 0 }, { "Metal", 6, 3, 0 } };
        const std::vector<MeshArtifactVertex> vertices = {
            { 0,0,0,  0,1,0,  0,0 }, { 1,0,0,  0,1,0,  1,0 },
            { 1,0,1,  0,1,0,  1,1 }, { 0,0,1,  0,1,0,  0,1 },
        };
        const std::vector<std::uint32_t> indices = { 0,1,2, 0,2,3, 1,2,3 };
        REQUIRE(WriteMeshArtifact(path, desc, vertices, indices));
        return ReadFileBinary(path);
    }
}

TEST_CASE("v1 texture and mesh artifacts match the checked-in bytes", "[artifact][baseline]")
{
    const auto root = RepoRoot() / "Tests/ArcaneTests/data/artifacts";
    for (const char* name : { "v1-texture.arcart", "v1-mesh.arcart" })
    {
        const auto path = root / name;
        REQUIRE(fs::exists(path));
        const std::string disk = ReadFileBinary(path);
        std::string expect = ReadFileBinary(root / (std::string(name) + ".sha256"));
        if (!expect.empty() && expect.back() == '\n') expect.pop_back();
        CHECK(Arcane::Crypto::Sha256Hex(disk) == expect);
        const auto fresh = (std::string(name) == "v1-texture.arcart")
            ? WriteV1TextureToTemp() : WriteV1MeshToTemp();
        CHECK(fresh == disk);
    }
}

TEST_CASE("capture v1 artifacts", "[.][artifact-baseline]")
{
    const char* arm = std::getenv("ARCANE_CAPTURE_PHYSICS_BASELINE");
    if (!arm || arm[0] != '1') return;
    const auto root = RepoRoot() / "Tests/ArcaneTests/data/artifacts";
    fs::create_directories(root);
    const std::string tex = WriteV1TextureToTemp();
    const std::string mesh = WriteV1MeshToTemp();
    WriteFileBinary(root / "v1-texture.arcart", tex);
    WriteFileBinary(root / "v1-texture.arcart.sha256", Arcane::Crypto::Sha256Hex(tex) + "\n");
    WriteFileBinary(root / "v1-mesh.arcart", mesh);
    WriteFileBinary(root / "v1-mesh.arcart.sha256", Arcane::Crypto::Sha256Hex(mesh) + "\n");
}
