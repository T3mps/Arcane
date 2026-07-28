// SpriteAssetTest.cpp -- .arcsprite JSON I/O + geometry math. CPU-only, no GPU.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>
#include <Arcane/Project/AssetRegistry.hpp>
#include <filesystem>
#include <fstream>

namespace
{
    std::filesystem::path TempDir(const char* leaf)
    {
        auto d = std::filesystem::temp_directory_path() / "arcane_sprite_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }
}

TEST_CASE("SpriteAsset round-trips every field", "[sprite]")
{
    const auto dir = TempDir("roundtrip");
    Arcane::SpriteAssetData data;
    data.id         = Arcane::Guid::Generate();
    data.name       = "hero";
    data.texture    = Arcane::Guid::Generate();
    data.ppu        = 64.0f;
    data.sourcePos  = {16.0f, 32.0f};
    data.sourceSize = {48.0f, 24.0f};
    data.pivot      = {0.25f, 1.0f};
    REQUIRE(Arcane::SaveSpriteAsset(dir / "hero.arcsprite", data));
    const auto back = Arcane::LoadSpriteAsset(dir / "hero.arcsprite");
    REQUIRE(back.has_value());
    CHECK(back->id == data.id);
    CHECK(back->name == "hero");
    CHECK(back->texture == data.texture);
    CHECK(back->ppu == 64.0f);
    CHECK(back->sourcePos == data.sourcePos);
    CHECK(back->sourceSize == data.sourceSize);
    CHECK(back->pivot == data.pivot);
}

TEST_CASE("SpriteAsset absent fields take defaults", "[sprite]")
{
    const auto dir = TempDir("defaults");
    {
        std::ofstream out(dir / "min.arcsprite", std::ios::binary);
        out << R"({"id":"0123456789abcdef0123456789abcdef","type":"sprite"})";
    }
    const auto back = Arcane::LoadSpriteAsset(dir / "min.arcsprite");
    REQUIRE(back.has_value());
    CHECK_FALSE(back->texture.IsValid());
    CHECK(back->ppu == 100.0f);
    CHECK(back->sourcePos == glm::vec2(0.0f));
    CHECK(back->sourceSize == glm::vec2(0.0f));
    CHECK(back->pivot == glm::vec2(0.5f));
}

TEST_CASE("SpriteAsset load rejects non-sprite json and clamps bad ppu", "[sprite]")
{
    const auto dir = TempDir("reject");
    {
        std::ofstream out(dir / "not.arcsprite", std::ios::binary);
        out << R"({"hello":"world"})";
    }
    CHECK_FALSE(Arcane::LoadSpriteAsset(dir / "not.arcsprite").has_value());
    {
        std::ofstream out(dir / "badppu.arcsprite", std::ios::binary);
        out << R"({"id":"0123456789abcdef0123456789abcdef","type":"sprite","ppu":0.0})";
    }
    const auto back = Arcane::LoadSpriteAsset(dir / "badppu.arcsprite");
    REQUIRE(back.has_value());
    CHECK(back->ppu == 100.0f);   // ppu <= 0 falls back to the default, warned
}

TEST_CASE("ComputeSpriteGeom full texture and sub-rect", "[sprite]")
{
    Arcane::SpriteAssetData data;   // defaults: full rect, ppu 100
    auto g = Arcane::ComputeSpriteGeom(data, 200, 50);
    CHECK(g.uvMin == glm::vec2(0.0f));
    CHECK(g.uvMax == glm::vec2(1.0f));
    CHECK(g.sizeMeters == glm::vec2(2.0f, 0.5f));   // 200/100, 50/100

    data.sourcePos  = {50.0f, 10.0f};
    data.sourceSize = {100.0f, 25.0f};
    g = Arcane::ComputeSpriteGeom(data, 200, 50);
    CHECK(g.uvMin == glm::vec2(0.25f, 0.2f));       // 50/200, 10/50
    CHECK(g.uvMax == glm::vec2(0.75f, 0.7f));       // 150/200, 35/50
    CHECK(g.sizeMeters == glm::vec2(1.0f, 0.25f));  // 100/100, 25/100

    g = Arcane::ComputeSpriteGeom(data, 0, 0);      // no texture dims yet
    CHECK(g.sizeMeters == glm::vec2(1.0f));         // safe fallback, never NaN/0
}

TEST_CASE("AssetRegistry scans and AddFiles .arcsprite as a native asset", "[sprite][project]")
{
    const auto dir = TempDir("registry");
    std::filesystem::create_directories(dir / "Content");
    Arcane::SpriteAssetData data;
    data.id = Arcane::Guid::Generate();
    REQUIRE(Arcane::SaveSpriteAsset(dir / "Content" / "s.arcsprite", data));

    Arcane::AssetRegistry reg;
    reg.ScanContent(dir / "Content", "game");
    CHECK(reg.Resolve(data.id).has_value());
    CHECK(*reg.Resolve(data.id) == "game://s.arcsprite");
}
