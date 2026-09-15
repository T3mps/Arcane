// Mesh-ASSET thumbnail harvesting, on a real adapter -- final-review C1's net.
//
// WHY THIS IS A [gpu] CASE AND NOT A DEVICE-LESS ONE: the bug lives entirely in
// the render half. MaterialPreviewHarvester::Harvest stashes the resolved
// geometry, names a guid on the MeshInstances, and hands both to a real
// NriGraphContext whose NriMeshBufferCache caches BY GUID and consults the
// supply only on a MISS. There is no seam between "which guid the instances
// name" and "which buffers get bound" short of the device, so the only honest
// instrument is the picture.
//
// THE ASSERTION IS A CONTROL, NOT "the two pictures differ". Two different
// meshes produce different pictures even WITH the bug, because FrameMeshBounds
// re-frames the camera per asset -- so "differs" proves nothing. Instead the
// cube is harvested twice: once on a vehicle that has seen nothing else, and
// once as the SECOND harvest on a vehicle that already holds another mesh's
// residency. Those two pictures must be identical. Under the pre-fix code
// (every mesh harvested under one session-fixed 'MESH' guid) the second harvest
// HIT the sphere's resident buffers and drew the first 36 of the sphere's
// indices, so the two cube pictures differed.
//
// The two fixtures are chosen to share an AABB on purpose -- BuildCube(1.0f) and
// BuildUvSphere(0.5f, ...) both span [-0.5, 0.5]^3 (MeshAsset.cpp's unit rule) --
// so FrameMeshBounds hands both harvests the SAME camera and the picture is a
// function of the geometry alone.

// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/OffscreenVehicle.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#include <Project/MaterialPreviewHarvester.hpp>

#undef ERROR

#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "Helpers/GpuCapability.hpp"

namespace fs = std::filesystem;

namespace
{
    using Arcane::Guid;
    using Arcane::Editor::MaterialPreviewHarvester;

    fs::path FreshDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_mesh_thumb_harvest" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    Guid WriteMesh(const fs::path& file, Arcane::MeshSource source, const char* name)
    {
        Arcane::MeshAssetData d;
        d.id     = Guid::Generate();
        d.name   = name;
        d.source = source;
        REQUIRE(Arcane::SaveMeshAsset(file, d));
        return d.id;
    }

    std::vector<unsigned char> ReadAll(const fs::path& p)
    {
        std::vector<unsigned char> bytes;
        std::error_code ec;
        const auto size = fs::file_size(p, ec);
        if (ec)
            return bytes;
        bytes.resize(static_cast<std::size_t>(size));
        std::FILE* f = std::fopen(p.string().c_str(), "rb");
        if (!f)
            return {};
        const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        bytes.resize(read);
        return bytes;
    }

    // Drive the harvester until its queue drains. Two Pumps is the steady cost of
    // one mesh (start, then harvest); the bound is generous so a Retry cannot hang
    // the suite.
    void Drain(MaterialPreviewHarvester& h, double& clock)
    {
        for (int i = 0; i < 12 && h.PendingCount() != 0; ++i)
        {
            h.Pump(clock);
            clock += 1.0 / 60.0;
        }
        CHECK(h.PendingCount() == 0u);
    }
}

TEST_CASE("pixel: two meshes harvested through ONE preview vehicle each render their "
          "OWN geometry", "[gpu][thumbs]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);

    Arcane::HostConfig cfg;
    cfg.backend  = Arcane::GraphicsBackend::D3D12;
    cfg.headless = true;
    auto chrome = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(chrome != nullptr);

    const fs::path assets = FreshDir("assets");
    const fs::path thumbsControl = FreshDir("thumbs_control");
    const fs::path thumbsShared  = FreshDir("thumbs_shared");

    const fs::path cubeFile   = assets / "cube.arcmesh";
    const fs::path sphereFile = assets / "sphere.arcmesh";
    const Guid cubeId   = WriteMesh(cubeFile,   Arcane::MeshSource::Cube,     "probe-cube");
    const Guid sphereId = WriteMesh(sphereFile, Arcane::MeshSource::UvSphere, "probe-sphere");

    const auto makeServices = [&](const fs::path& thumbs)
    {
        MaterialPreviewHarvester::Services s;
        s.chromeGraph = [&]() { return &chrome->Graph(); };
        s.hostConfig  = &cfg;
        s.backend     = Arcane::GraphicsBackend::D3D12;
        // No compiler and no source provider: a mesh asset compiles nothing, which
        // is the whole reason Subject::Mesh has no `pending` stage.
        s.resolveAsset = [&](const Guid& g) -> std::optional<fs::path>
        {
            if (g == cubeId)   return cubeFile;
            if (g == sphereId) return sphereFile;
            return std::nullopt;
        };
        s.thumbnailDir = [thumbs]() { return thumbs; };
        return s;
    };

    const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
    double clock = 0.0;

    // ---- THE CONTROL: the cube, alone, on a vehicle that has seen nothing ----
    std::vector<unsigned char> cubeAlone;
    {
        MaterialPreviewHarvester h(makeServices(thumbsControl));
        chrome->Graph().SetPixelSupply(
            [&](const Guid& g) { return h.PixelsForThumb(g); });
        h.RequestMesh(cubeId);
        Drain(h, clock);
        h.Shutdown();
        // Before `h` dies: the chrome context outlives it and still holds a lambda
        // capturing it.
        chrome->Graph().SetPixelSupply(nullptr);
        cubeAlone = ReadAll(thumbsControl / (cubeId.ToString() + ".png"));
        REQUIRE(!cubeAlone.empty());
    }

    // ---- THE SUBJECT: sphere FIRST, then the cube, through ONE harvester ----
    std::vector<unsigned char> sphereShared, cubeShared;
    {
        MaterialPreviewHarvester h(makeServices(thumbsShared));
        chrome->Graph().SetPixelSupply(
            [&](const Guid& g) { return h.PixelsForThumb(g); });

        h.RequestMesh(sphereId);
        Drain(h, clock);
        sphereShared = ReadAll(thumbsShared / (sphereId.ToString() + ".png"));
        REQUIRE(!sphereShared.empty());

        h.RequestMesh(cubeId);
        Drain(h, clock);
        h.Shutdown();
        chrome->Graph().SetPixelSupply(nullptr);
        cubeShared = ReadAll(thumbsShared / (cubeId.ToString() + ".png"));
        REQUIRE(!cubeShared.empty());
    }

    // THE ASSERTION (final-review C1): the second harvest through a shared vehicle
    // draws the CUBE, pixel for pixel the same picture the cube gets alone.
    CHECK(cubeShared == cubeAlone);
    // ...and the two assets really are different pictures, so the check above is
    // pinning "the right geometry" rather than "everything looks the same".
    CHECK(sphereShared != cubeAlone);

    // No OOB index read, no refused upload, no validation error on either harvest.
    CHECK(Arcane::RenderErrorCount() == errorsBefore);

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "arcane_mesh_thumb_harvest", ec);
}
