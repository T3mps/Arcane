// Shader artifacts load on both backends. (The hot-reload mechanism test
// joins this file in the hot-reload task.)

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    void CheckShaderLoads(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);

        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                     "data/shaders");
        REQUIRE(shaders != nullptr);
        REQUIRE(shaders->Generation() == 1);

        REQUIRE(shaders->Get("sprite_vs", nvrhi::ShaderType::Vertex) != nullptr);
        REQUIRE(shaders->Get("sprite_ps", nvrhi::ShaderType::Pixel) != nullptr);
        REQUIRE(shaders->Get("circle_vs", nvrhi::ShaderType::Vertex) != nullptr);
        REQUIRE(shaders->Get("circle_ps", nvrhi::ShaderType::Pixel) != nullptr);
        REQUIRE(shaders->Get("tonemap_vs", nvrhi::ShaderType::Vertex) != nullptr);
        REQUIRE(shaders->Get("tonemap_ps", nvrhi::ShaderType::Pixel) != nullptr);

        // Cached: same handle back.
        REQUIRE(shaders->Get("sprite_vs", nvrhi::ShaderType::Vertex) ==
                shaders->Get("sprite_vs", nvrhi::ShaderType::Vertex));

        // Missing artifact: null, not a crash. (Logs one ARC_ERROR -- that is
        // an engine log, not an NVRHI validation error; RenderErrorCount is
        // unaffected.)
        REQUIRE(shaders->Get("does_not_exist", nvrhi::ShaderType::Pixel) == nullptr);

        CHECK(Arcane::RenderErrorCount() == 0);
    }

    // Copies the shader artifact tree to a temp dir so the test can touch
    // files without disturbing the build output other tests read.
    //
    // Adaptation vs. spec: the spec's CopyShaderTree uses
    //   std::filesystem::copy("data/shaders", dst, ...)
    // which resolves "data/shaders" against the CWD, while ShaderLibrary::Create
    // resolves relative dirs against the EXE directory (GetModuleFileNameW).
    // Tests run from the repo root, not the exe dir, so the CWD path does not
    // find the artifacts. We replicate the same resolution here: build the
    // source from the exe's parent / "data/shaders" so the copy matches what
    // ShaderLibrary::Create would load. The destination is an absolute path
    // (temp_directory_path()/"arcane-hotreload"), and passing an absolute path
    // to ShaderLibrary::Create bypasses exe-relative resolution by design.
    std::filesystem::path CopyShaderTree()
    {
        std::filesystem::path exeDir;
#ifdef _WIN32
        wchar_t modulePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
            exeDir = std::filesystem::path(modulePath).parent_path();
#endif
        const auto src = exeDir / "data/shaders";
        const auto dst = std::filesystem::temp_directory_path() / "arcane-hotreload";
        std::filesystem::remove_all(dst);
        std::filesystem::copy(src, dst,
                              std::filesystem::copy_options::recursive);
        return dst;
    }
}

TEST_CASE("d3d12: shader artifacts load", "[gpu][d3d12]")
{
    CheckShaderLoads(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: shader artifacts load", "[gpu][vulkan]")
{
    CheckShaderLoads(Arcane::GraphicsBackend::Vulkan);
}

TEST_CASE("shader hot reload: mtime change reloads and bumps generation", "[gpu][d3d12]")
{
    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;
    auto device = Arcane::RenderDevice::Create(desc);
    REQUIRE(device != nullptr);

    const auto dir = CopyShaderTree();
    auto shaders = Arcane::ShaderLibrary::Create(
        device->Nvrhi(), Arcane::GraphicsBackend::D3D12, dir);
    REQUIRE(shaders != nullptr);

    nvrhi::ShaderHandle before =
        shaders->Get("sprite_ps", nvrhi::ShaderType::Pixel);
    REQUIRE(before != nullptr);
    REQUIRE(shaders->Generation() == 1);

    // No change: Poll is a no-op.
    REQUIRE_FALSE(shaders->Poll());
    REQUIRE(shaders->Generation() == 1);

    // Bump the artifact's mtime past filesystem timestamp granularity.
    const auto artifact = dir / "dxil" / "sprite_ps.bin";
    std::filesystem::last_write_time(
        artifact, std::filesystem::file_time_type::clock::now() +
                      std::chrono::seconds(2));

    REQUIRE(shaders->Poll());
    REQUIRE(shaders->Generation() == 2);
    nvrhi::ShaderHandle after =
        shaders->Get("sprite_ps", nvrhi::ShaderType::Pixel);
    REQUIRE(after != nullptr);
    REQUIRE(after != before);  // fresh handle, old one still validly held

    CHECK(Arcane::RenderErrorCount() == 0);
}
