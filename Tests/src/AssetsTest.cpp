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
#include <optional>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Project/Project.hpp>
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
    std::filesystem::path WriteTestJson(const std::string& content,
                                        const char* name = "arcane-assets-test.json")
    {
        const auto path =
            std::filesystem::temp_directory_path() / name;
        std::ofstream f(path, std::ios::binary);
        REQUIRE(f.is_open());
        f << content;
        return path;
    }
}

TEST_CASE("assets: byte budget evicts least-recently-used entries and stays within budget", "[assets][budget]")
{
    // Three 40-byte files against a 100-byte budget: only two fit at a time.
    // Device-free: budget/eviction runs on the shared facade path, exercised
    // here through the bytes loader.
    const auto a = WriteTestJson(std::string(40, 'a'), "arcane-assets-budget-a.bin");
    const auto b = WriteTestJson(std::string(40, 'b'), "arcane-assets-budget-b.bin");
    const auto c = WriteTestJson(std::string(40, 'c'), "arcane-assets-budget-c.bin");

    Arcane::AssetsDesc desc;
    desc.byteBudget = 100;
    auto assets = Arcane::Assets::Create(nullptr, desc);
    REQUIRE(assets != nullptr);

    auto pa = assets->GetBytes(a);
    auto pb = assets->GetBytes(b);
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    CHECK(assets->Stats().totalBytes == 80);   // both fit

    CHECK(assets->GetBytes(a) == pa);          // touch A: B is now the LRU

    auto pc = assets->GetBytes(c);             // 120 > 100 -> evicts B (not A)
    REQUIRE(pc != nullptr);
    CHECK(assets->Stats().totalBytes == 80);
    CHECK(assets->Stats().count == 2);

    CHECK(assets->GetBytes(a) == pa);          // A survived (recently used)
    auto pb2 = assets->GetBytes(b);            // B was evicted: fresh load...
    REQUIRE(pb2 != nullptr);
    CHECK(pb2 != pb);                          // ...(pb still held, so a new alloc)
    CHECK(assets->Stats().totalBytes <= 100);  // never settles over budget

    std::remove(a.string().c_str());
    std::remove(b.string().c_str());
    std::remove(c.string().c_str());
}

TEST_CASE("assets: budget is facade-wide -- a JSON load evicts older bytes entries", "[assets][budget]")
{
    const auto a = WriteTestJson(std::string(40, 'a'), "arcane-assets-budget-d.bin");
    const auto b = WriteTestJson(std::string(40, 'b'), "arcane-assets-budget-e.bin");
    // Exactly 40 bytes of valid JSON: 7 ({"k":1}) + 33 trailing spaces
    // (legal whitespace).
    const auto j = WriteTestJson(R"({"k":1})" + std::string(33, ' '),
                                 "arcane-assets-budget-f.json");

    Arcane::AssetsDesc desc;
    desc.byteBudget = 100;
    auto assets = Arcane::Assets::Create(nullptr, desc);
    REQUIRE(assets != nullptr);

    auto pa = assets->GetBytes(a);
    auto pb = assets->GetBytes(b);
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);

    // The JSON insert pushes the FACADE total to 120: the global LRU is the
    // bytes entry A -- it lives in the OTHER cache, and is still evicted.
    auto pj = assets->GetJson(j);
    REQUIRE(pj != nullptr);
    CHECK(assets->Stats().totalBytes == 80);
    CHECK(assets->GetBytes(b) == pb);          // survived
    CHECK(assets->GetJson(j) == pj);           // survived

    auto pa2 = assets->GetBytes(a);            // A reloads: it was evicted
    REQUIRE(pa2 != nullptr);
    CHECK(pa2 != pa);
    CHECK(assets->Stats().totalBytes <= 100);

    std::remove(a.string().c_str());
    std::remove(b.string().c_str());
    std::remove(j.string().c_str());
}

TEST_CASE("assets: an asset larger than the whole budget is served but not cached", "[assets][budget]")
{
    const auto big = WriteTestJson(std::string(60, 'x'), "arcane-assets-budget-g.bin");

    Arcane::AssetsDesc desc;
    desc.byteBudget = 50;
    auto assets = Arcane::Assets::Create(nullptr, desc);
    REQUIRE(assets != nullptr);

    // The load succeeds (the caller holds the bytes) but the budget is
    // strict: the oversized entry is swept right back out, so the facade
    // never settles above budget.
    auto p1 = assets->GetBytes(big);
    REQUIRE(p1 != nullptr);
    CHECK(p1->size() == 60);
    CHECK(assets->Stats().totalBytes <= 50);

    auto p2 = assets->GetBytes(big);           // not cached: a fresh load
    REQUIRE(p2 != nullptr);
    CHECK(p2 != p1);
    CHECK(*p2 == *p1);

    std::remove(big.string().c_str());
}

TEST_CASE("assets: memoized failures are ~zero cost and survive the budget sweep", "[assets][budget]")
{
    const auto a = WriteTestJson(std::string(40, 'a'), "arcane-assets-budget-h.bin");
    const auto b = WriteTestJson(std::string(40, 'b'), "arcane-assets-budget-i.bin");

    Arcane::AssetsDesc desc;
    desc.byteBudget = 50;
    auto assets = Arcane::Assets::Create(nullptr, desc);
    REQUIRE(assets != nullptr);

    CHECK(assets->GetBytes("does/not/exist.bin") == nullptr);  // memoized, 0 bytes
    CHECK(assets->Stats().count == 1);

    auto pa = assets->GetBytes(a);
    REQUIRE(pa != nullptr);
    auto pb = assets->GetBytes(b);             // 80 > 50 -> evicts A...
    REQUIRE(pb != nullptr);

    // ...but NOT the failure memo: entries = {failure, B}, bytes = B only.
    CHECK(assets->Stats().count == 2);
    CHECK(assets->Stats().totalBytes == 40);

    std::remove(a.string().c_str());
    std::remove(b.string().c_str());
}

TEST_CASE("assets: byteBudget 0 opts out of eviction; default is 256 MiB", "[assets][budget]")
{
    // Documentation-lock on the default so a drive-by change trips a test.
    CHECK(Arcane::AssetsDesc{}.byteBudget == 256ull * 1024 * 1024);

    const auto a = WriteTestJson(std::string(40, 'a'), "arcane-assets-budget-j.bin");
    const auto b = WriteTestJson(std::string(40, 'b'), "arcane-assets-budget-k.bin");
    const auto c = WriteTestJson(std::string(40, 'c'), "arcane-assets-budget-l.bin");

    Arcane::AssetsDesc desc;
    desc.byteBudget = 0;                       // unbounded (the legacy contract)
    auto assets = Arcane::Assets::Create(nullptr, desc);
    REQUIRE(assets != nullptr);

    auto pa = assets->GetBytes(a);
    auto pb = assets->GetBytes(b);
    auto pc = assets->GetBytes(c);
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    REQUIRE(pc != nullptr);
    CHECK(assets->Stats().totalBytes == 120);  // nothing evicted
    CHECK(assets->Stats().count == 3);

    std::remove(a.string().c_str());
    std::remove(b.string().c_str());
    std::remove(c.string().c_str());
}

TEST_CASE("assets: GetTexture before a render device is set returns null (no crash)", "[assets]")
{
    // Facade built device-less (the headless / before-SetRenderResources path).
    // GetTexture must degrade to null instead of dereferencing a null device.
    auto assets = Arcane::Assets::Create(nullptr);
    REQUIRE(assets != nullptr);

    const auto png = WriteTestPng();          // a real, loadable PNG on disk...
    CHECK(assets->GetTexture(png) == nullptr); // ...still null: no device to upload to
    CHECK(assets->GetTexture(png) == nullptr); // memoized: second call also null, no retry

    // A missing path is likewise null (and never reaches stbi_load).
    CHECK(assets->GetTexture("does/not/exist.png") == nullptr);

    std::remove(png.string().c_str());
}

TEST_CASE("assets: SetDevice binds a device to a device-less facade", "[gpu][d3d12][assets]")
{
    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;
    auto device = Arcane::RenderDevice::Create(desc);
    REQUIRE(device != nullptr);

    // Created with NO device: GetTexture is null and the miss is memoized.
    auto assets = Arcane::Assets::Create(nullptr);
    REQUIRE(assets != nullptr);

    const auto png = WriteTestPng();
    CHECK(assets->GetTexture(png) == nullptr);

    // Bind the device late (mirrors Runtime::SetRenderResources). SetDevice clears
    // the device-less failure memo, so the same path now loads for real.
    assets->SetDevice(device->Nvrhi());
    nvrhi::TextureHandle tex = assets->GetTexture(png);
    REQUIRE(tex != nullptr);
    CHECK(tex->getDesc().width == 2);

    CHECK(Arcane::RenderErrorCount() == 0);
    std::remove(png.string().c_str());
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

TEST_CASE("Assets content-root anchors relative loads", "[assets]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_contentroot";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    std::ofstream(root / "probe.json", std::ios::binary) << R"({"ok":true})";

    auto assets = Arcane::Assets::Create(nullptr);
    assets->SetContentRoot(root);

    auto doc = assets->GetJson("probe.json");   // relative -> resolves under root
    REQUIRE(doc != nullptr);
    REQUIRE((*doc)["ok"].get<bool>() == true);

    auto abs = assets->GetJson(root / "probe.json");   // absolute bypasses the root
    REQUIRE(abs != nullptr);

    fs::remove_all(root, ec);
}

TEST_CASE("Assets resolves AssetId loads through the installed resolver", "[assets]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_guid";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    std::ofstream(root / "mat.json", std::ios::binary) << R"({"kind":"material"})";
    std::ofstream(root / "blob.bin", std::ios::binary) << "0123456789";

    const Arcane::Guid jsonId = Arcane::Guid::Generate();
    const Arcane::Guid binId = Arcane::Guid::Generate();

    auto assets = Arcane::Assets::Create(nullptr);

    SECTION("no resolver installed -> null (one warning, no crash)")
    {
        CHECK(assets->GetJson(Arcane::AssetId::FromGuid(jsonId)) == nullptr);
        CHECK(assets->GetJson(Arcane::AssetId::FromGuid(jsonId)) == nullptr);
    }

    SECTION("resolver routes ids into the same cached loaders")
    {
        assets->SetAssetResolver(
            [&](const Arcane::AssetId& id) -> std::optional<fs::path>
            {
                if (id.Value() == jsonId) return root / "mat.json";
                if (id.Value() == binId)  return root / "blob.bin";
                return std::nullopt;
            });

        auto doc = assets->GetJson(Arcane::AssetId::FromGuid(jsonId));
        REQUIRE(doc != nullptr);
        CHECK((*doc)["kind"].get<std::string>() == "material");

        // Same file by id and by path share ONE cache entry (same shared_ptr).
        auto byPath = assets->GetJson(root / "mat.json");
        CHECK(byPath.get() == doc.get());

        auto bytes = assets->GetBytes(Arcane::AssetId::FromGuid(binId));
        REQUIRE(bytes != nullptr);
        CHECK(bytes->size() == 10);

        // Unknown id -> null, memoized warn-once. Nil id -> null.
        CHECK(assets->GetJson(Arcane::AssetId::FromGuid(Arcane::Guid::Generate())) == nullptr);
        CHECK(assets->GetJson(Arcane::AssetId{}) == nullptr);

        // Texture by id without a device: resolves, then degrades to null
        // exactly like the path overload (no crash headless).
        CHECK(assets->GetTexture(Arcane::AssetId::FromGuid(binId)) == nullptr);
    }

    SECTION("a real Project's ResolveAsset plugs in as the resolver")
    {
        // Scaffold a minimal project whose Content/ carries a native asset with
        // an embedded id -- the exact wiring Runtime::OpenProject installs.
        const fs::path projDir = root / "proj";
        fs::create_directories(projDir / "Content");
        std::ofstream(projDir / "Game.arcproj", std::ios::binary)
            << R"({ "formatVersion": 1, "name": "Game", "engine": { "abi": 5 } })";
        std::ofstream(projDir / "Content" / "thing.json", std::ios::binary)
            << R"({ "id": "b7e0c1de-2222-4333-8444-555566667777", "payload": 7 })";

        auto proj = Arcane::Project::Open(projDir);
        REQUIRE(proj.has_value());
        assets->SetAssetResolver(
            [p = &*proj](const Arcane::AssetId& id) { return p->ResolveAsset(id); });

        const auto g = Arcane::Guid::FromString("b7e0c1de-2222-4333-8444-555566667777");
        REQUIRE(g.has_value());
        auto doc = assets->GetJson(Arcane::AssetId::FromGuid(*g));
        REQUIRE(doc != nullptr);
        CHECK((*doc)["payload"].get<int>() == 7);
    }

    fs::remove_all(root, ec);
}

TEST_CASE("assets: json loader -- parse, cache identity, memoized failure", "[gpu][d3d12][assets]")
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

    // Malformed JSON: null both times. Use a distinct filename so this path
    // has no prior cache entry from the valid-JSON write above.
    const auto badPath = WriteTestJson("{ not valid json !!!}",
                                       "arcane-assets-test-bad.json");
    CHECK(assets->GetJson(badPath) == nullptr);
    CHECK(assets->GetJson(badPath) == nullptr);

    std::remove(jsonPath.string().c_str());
    std::remove(badPath.string().c_str());
}
