// Assets facade: PNG -> sRGB nvrhi texture with byte-true readback (the
// sRGB format must round-trip raw bytes exactly on copy), bytes/json
// loaders, and memoized failure on a missing path.

#include <catch2/catch_test_macros.hpp>

// stb_image_write: implementation is owned by VendorSmokeTest.cpp in this
// same exe (STB_IMAGE_WRITE_IMPLEMENTATION defined there). Include header only.
#include <stb_image_write.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Render/Device.hpp>

namespace
{
    // Writes a 2x2 PNG with known bytes into the temp dir.
    std::filesystem::path WriteTestPng()
    {
        const unsigned char pixels[2 * 2 * 4] = {
            255, 0, 0, 255,   0, 255, 0, 255,
            0, 0, 255, 255,   128, 64, 32, 255,
        };
        const auto path =
            std::filesystem::temp_directory_path() / "arcane-assets-test.png";
        REQUIRE(stbi_write_png(path.string().c_str(), 2, 2, 4, pixels, 8) != 0);
        return path;
    }

    // Writes a temp JSON file; returns its path.
    std::filesystem::path WriteTestJson(const std::string& content)
    {
        const auto path =
            std::filesystem::temp_directory_path() / "arcane-assets-test.json";
        std::ofstream f(path, std::ios::binary);
        REQUIRE(f.is_open());
        f << content;
        return path;
    }
}

TEST_CASE("assets: texture loads as sRGB and reads back byte-true", "[gpu][d3d12][assets]")
{
    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;
    auto device = Arcane::RenderDevice::Create(desc);
    REQUIRE(device != nullptr);
    auto assets = Arcane::Assets::Create(device->Nvrhi());
    REQUIRE(assets != nullptr);

    const auto png = WriteTestPng();
    nvrhi::TextureHandle tex = assets->GetTexture(png);
    REQUIRE(tex != nullptr);
    // nvrhi::Format::SRGBA8_UNORM confirmed in nvrhi.h line 186.
    CHECK(tex->getDesc().format == nvrhi::Format::SRGBA8_UNORM);
    CHECK(tex->getDesc().width == 2);

    // Cached: identical handle.
    CHECK(assets->GetTexture(png) == tex);

    // Copy to staging and compare raw bytes (copy does not convert).
    auto stagingDesc = nvrhi::TextureDesc()
        .setWidth(2).setHeight(2)
        .setFormat(nvrhi::Format::SRGBA8_UNORM)
        .setDebugName("AssetsReadback");
    auto staging = device->Nvrhi()->createStagingTexture(
        stagingDesc, nvrhi::CpuAccessMode::Read);
    auto commandList = device->Nvrhi()->createCommandList();
    commandList->open();
    commandList->copyTexture(staging, nvrhi::TextureSlice(),
                             tex, nvrhi::TextureSlice());
    commandList->close();
    device->Nvrhi()->executeCommandList(commandList);
    device->Nvrhi()->waitForIdle();

    size_t rowPitch = 0;
    const auto* pixels = static_cast<const uint8_t*>(
        device->Nvrhi()->mapStagingTexture(staging, nvrhi::TextureSlice(),
                                           nvrhi::CpuAccessMode::Read, &rowPitch));
    REQUIRE(pixels != nullptr);
    CHECK((int)pixels[0] == 255);             // texel (0,0) R
    CHECK((int)pixels[rowPitch + 4] == 128);  // row 1, second texel, R
    device->Nvrhi()->unmapStagingTexture(staging);
    device->Nvrhi()->runGarbageCollection();

    // Memoized failure: a missing path returns null both times (one
    // ARC_WARN log, no retry storm). ARC_WARN does not increment
    // RenderErrorCount() -- only NvrhiMessageCallback (nvrhi validation
    // errors) does. Asset failures use ARC_WARN so the GPU test's
    // RenderErrorCount() == 0 assertion stays clean.
    CHECK(assets->GetTexture("does/not/exist.png") == nullptr);
    CHECK(assets->GetTexture("does/not/exist.png") == nullptr);

    auto bytes = assets->GetBytes("data/fonts/Roboto-Regular.ttf");
    REQUIRE(bytes != nullptr);
    CHECK(bytes->size() > 10000);

    CHECK(Arcane::RenderErrorCount() == 0);
    std::remove(png.string().c_str());
}

TEST_CASE("assets: json loader -- parse, cache identity, memoized failure", "[assets]")
{
    // JSON test: no GPU required; uses a null device since Assets::Create
    // accepts IDevice* and only needs it for texture uploads.
    // Use a real device anyway so the test is straightforward.
    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;
    auto device = Arcane::RenderDevice::Create(desc);
    REQUIRE(device != nullptr);
    auto assets = Arcane::Assets::Create(device->Nvrhi());
    REQUIRE(assets != nullptr);

    // Valid JSON: second call returns the exact same shared_ptr.
    const auto jsonPath = WriteTestJson(R"({"hello":"world","n":42})");
    auto doc1 = assets->GetJson(jsonPath);
    REQUIRE(doc1 != nullptr);
    CHECK((*doc1)["hello"].get<std::string>() == "world");
    CHECK((*doc1)["n"].get<int>() == 42);

    auto doc2 = assets->GetJson(jsonPath);
    CHECK(doc2 == doc1);   // same shared_ptr (cached)

    // Missing file: null both times (memoized failure, no second attempt).
    CHECK(assets->GetJson("does/not/exist.json") == nullptr);
    CHECK(assets->GetJson("does/not/exist.json") == nullptr);

    // Malformed JSON: null both times.
    const auto badPath = WriteTestJson("{ not valid json !!!}");
    CHECK(assets->GetJson(badPath) == nullptr);
    CHECK(assets->GetJson(badPath) == nullptr);

    std::remove(jsonPath.string().c_str());
    std::remove(badPath.string().c_str());
}
