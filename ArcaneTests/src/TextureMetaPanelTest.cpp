// F2b Task 13: the Inspector's texture-asset settings block PURE half --
// ReadTextureMetaSettingsDisplay/WriteTextureMetaSettingsMerged
// (Panels/TextureMetaPanel.hpp). CPU-only: no ImGui, no EditorApp. The
// merge-preservation contract is the whole point of these tests -- a
// settings edit must never disturb the sidecar's `guid`/`version`/anything
// else already there, since AssetRegistry.cpp's ResolveSidecarId treats
// `guid` as the asset's durable identity.

#include <catch2/catch_test_macros.hpp>

#include <Panels/TextureMetaPanel.hpp>

#include <Json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace Arcane;
using namespace Arcane::Editor;
namespace fs = std::filesystem;

namespace
{
    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_texture_meta_panel_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    nlohmann::json ReadRawJson(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
    }
}

TEST_CASE("ReadTextureMetaSettingsDisplay defaults on a missing or texture-less .meta",
         "[editor]")
{
    const auto dir = TempDir("read_defaults");

    // Missing file entirely.
    const auto missing = dir / "nope.png.meta";
    const AssetPipeline::TextureMetaSettings none = ReadTextureMetaSettingsDisplay(missing);
    CHECK(none.format == AssetPipeline::TextureMetaSettings::Format::Auto);
    CHECK(none.srgb == true);
    CHECK(none.generateMips == true);
    CHECK(none.maxSize == 0);

    // A real sidecar with no "texture" block at all (the common case for a
    // freshly auto-imported .png -- AssetRegistry.cpp's ResolveSidecarId
    // mints exactly this shape).
    const auto plain = dir / "plain.png.meta";
    {
        nlohmann::json j;
        j["guid"] = "11112222-3333-4444-8555-666677778888";
        j["version"] = 1;
        std::ofstream out(plain, std::ios::binary);
        out << j.dump(2);
    }
    const AssetPipeline::TextureMetaSettings fromPlain = ReadTextureMetaSettingsDisplay(plain);
    CHECK(fromPlain.format == AssetPipeline::TextureMetaSettings::Format::Auto);
    CHECK(fromPlain.srgb == true);
    CHECK(fromPlain.generateMips == true);
    CHECK(fromPlain.maxSize == 0);
}

TEST_CASE("WriteTextureMetaSettingsMerged preserves guid/version/extra fields", "[editor]")
{
    const auto dir = TempDir("merge_write");
    const auto meta = dir / "hero.png.meta";

    // A sidecar carrying its identity PLUS a field this feature knows
    // nothing about -- the merge must not disturb either.
    {
        nlohmann::json j;
        j["guid"] = "aaaa1111-2222-4333-8444-555566667777";
        j["version"] = 1;
        j["someFutureField"] = "untouched";
        std::ofstream out(meta, std::ios::binary);
        out << j.dump(2);
    }

    AssetPipeline::TextureMetaSettings settings;
    settings.format = AssetPipeline::TextureMetaSettings::Format::Bc7;
    settings.srgb = false;
    settings.generateMips = false;
    settings.maxSize = 512;
    WriteTextureMetaSettingsMerged(meta, settings);

    const nlohmann::json raw = ReadRawJson(meta);
    REQUIRE(raw.is_object());
    CHECK(raw["guid"] == "aaaa1111-2222-4333-8444-555566667777");
    CHECK(raw["version"] == 1);
    CHECK(raw["someFutureField"] == "untouched");
    REQUIRE(raw.contains("texture"));
    CHECK(raw["texture"]["format"] == "Bc7");
    CHECK(raw["texture"]["srgb"] == false);
    CHECK(raw["texture"]["generateMips"] == false);
    CHECK(raw["texture"]["maxSize"] == 512);

    // Round-trips back through the reader too.
    const AssetPipeline::TextureMetaSettings back = ReadTextureMetaSettingsDisplay(meta);
    CHECK(back.format == AssetPipeline::TextureMetaSettings::Format::Bc7);
    CHECK(back.srgb == false);
    CHECK(back.generateMips == false);
    CHECK(back.maxSize == 512);
}

TEST_CASE("WriteTextureMetaSettingsMerged replaces an EXISTING texture block wholesale, "
         "leaving its siblings alone", "[editor]")
{
    const auto dir = TempDir("merge_overwrite");
    const auto meta = dir / "again.png.meta";

    {
        nlohmann::json j;
        j["guid"] = "cccc3333-4444-4555-8666-777788889999";
        j["version"] = 1;
        j["texture"]["format"] = "Rgba8";
        j["texture"]["srgb"] = true;
        j["texture"]["generateMips"] = true;
        j["texture"]["maxSize"] = 0;
        std::ofstream out(meta, std::ios::binary);
        out << j.dump(2);
    }

    AssetPipeline::TextureMetaSettings edited;
    edited.format = AssetPipeline::TextureMetaSettings::Format::Auto;
    edited.srgb = true;
    edited.generateMips = false;
    edited.maxSize = 2048;
    WriteTextureMetaSettingsMerged(meta, edited);

    const nlohmann::json raw = ReadRawJson(meta);
    CHECK(raw["guid"] == "cccc3333-4444-4555-8666-777788889999");
    CHECK(raw["texture"]["format"] == "Auto");
    CHECK(raw["texture"]["generateMips"] == false);
    CHECK(raw["texture"]["maxSize"] == 2048);
}

TEST_CASE("WriteTextureMetaSettingsMerged refuses a missing or non-object file "
         "rather than dropping the guid", "[editor]")
{
    const auto dir = TempDir("merge_refuse");

    // Missing entirely: refuses (no file materializes out of nothing).
    const auto missing = dir / "missing.png.meta";
    AssetPipeline::TextureMetaSettings settings;
    settings.maxSize = 1024;
    WriteTextureMetaSettingsMerged(missing, settings);
    CHECK_FALSE(fs::exists(missing));

    // Present but not a JSON object (a hand-corrupted or truncated file):
    // left byte-for-byte untouched, never overwritten with a bare
    // {"texture":...} that would drop whatever identity it used to carry.
    const auto corrupt = dir / "corrupt.png.meta";
    {
        std::ofstream out(corrupt, std::ios::binary);
        out << "not json at all {{{";
    }
    std::string before;
    {
        std::ifstream in(corrupt, std::ios::binary);
        before.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    WriteTextureMetaSettingsMerged(corrupt, settings);
    std::string after;
    {
        std::ifstream in(corrupt, std::ios::binary);
        after.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(before == after);
}
