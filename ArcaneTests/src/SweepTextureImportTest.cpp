// Settings arc S6-6: assets.import.texture.* -- TextureMetaSettings IS the
// settings struct (one literal source), and a texture's .meta "texture" block
// keeps only per-asset overrides: an ABSENT field resolves to the project's
// assets.import.texture.* value, in the cook (CookSession) and in the editor's
// import-settings rows alike.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"

#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>
#include <Arcane/Config/CVarRegistry.hpp>

#include "Panels/TextureMetaPanel.hpp"   // the editor's import-settings read/merge-write

#include <filesystem>
#include <fstream>

using Arcane::AssetPipeline::TextureMetaSettings;

TEST_CASE("sweep: assets.import.texture defaults are the struct's", "[sweep][texture-import]")
{
    const TextureMetaSettings d{};
    CHECK(d.format == TextureMetaSettings::Format::Auto);
    CHECK(d.srgb); CHECK(d.generateMips); CHECK(d.maxSize == 0u);
    Arcane::Test::RequireDefault("assets.import.texture.srgb", Arcane::CVarValue::Bool(true));
    Arcane::Test::RequireDefault("assets.import.texture.maxSize", Arcane::CVarValue::UInt32(0u));
    Arcane::Test::RequireDefault("assets.import.texture.generateMips", Arcane::CVarValue::Bool(true));
    Arcane::Test::RequireDefault("assets.import.texture.format", Arcane::CVarValue::Enum(0));   // Auto (ordinal, I7)

    // Inventory metadata: Editor audience, Project scope, Live.
    const Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    const auto meta = reg.Metadata(reg.Find("assets.import.texture.maxSize"));
    REQUIRE(meta.has_value());
    CHECK(meta->audience == Arcane::Audience::Editor);
    CHECK(meta->scope == Arcane::SettingScope::Project);
    CHECK(meta->apply == Arcane::ApplyMode::Live);
}

TEST_CASE("sweep: an absent .meta field takes the project default; a present one wins", "[sweep][texture-import]")
{
    TextureMetaSettings project{};
    project.srgb = false; project.maxSize = 1024;
    const auto absent = TextureMetaSettings::FromMetaJson(nlohmann::json::object(), project);
    CHECK_FALSE(absent.srgb);
    CHECK(absent.maxSize == 1024u);
    const auto present = TextureMetaSettings::FromMetaJson(nlohmann::json{ { "srgb", true } }, project);
    CHECK(present.srgb);
    CHECK(present.maxSize == 1024u);
}

TEST_CASE("sweep: FromMetaJson reports which fields the .meta itself set", "[sweep][texture-import]")
{
    TextureMetaSettings project{};
    project.format = TextureMetaSettings::Format::Rgba8;
    TextureMetaSettings::FieldsSet set;
    const nlohmann::json block{ { "srgb", false }, { "maxSize", "big" }, { "format", "nonsense" } };
    const auto resolved = TextureMetaSettings::FromMetaJson(block, project, &set);
    CHECK(set.srgb);
    CHECK_FALSE(set.generateMips);   // absent
    CHECK_FALSE(set.maxSize);        // mistyped: the project default stands
    CHECK_FALSE(set.format);         // unrecognised: the project default stands
    CHECK(resolved.format == TextureMetaSettings::Format::Rgba8);
    CHECK_FALSE(resolved.srgb);
}

TEST_CASE("sweep: the editor's merge-write keeps project-default fields absent", "[sweep][texture-import][editor]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_sweep_texture_import";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path meta = dir / "brick.png.meta";
    {
        std::ofstream out(meta, std::ios::binary);
        out << R"({ "guid": "00000000-0000-0000-0000-000000000001", "version": 1 })";
    }

    TextureMetaSettings project{};
    project.maxSize = 512;
    TextureMetaSettings::FieldsSet set;
    TextureMetaSettings shown = Arcane::Editor::ReadTextureMetaSettingsDisplay(meta, project, &set);
    CHECK(shown.maxSize == 512u);   // the absent field shows the project's value
    CHECK_FALSE(set.maxSize);

    // Toggle sRGB only: the block gains "srgb" and nothing else.
    shown.srgb = false;
    TextureMetaSettings::FieldsSet write = set;
    write.srgb = true;
    Arcane::Editor::WriteTextureMetaSettingsMerged(meta, shown, &write);

    const nlohmann::json doc = [&] {
        std::ifstream in(meta, std::ios::binary);
        return nlohmann::json::parse(in);
    }();
    REQUIRE(doc.contains("texture"));
    CHECK(doc["texture"] == nlohmann::json{ { "srgb", false } });
    CHECK(doc["guid"] == "00000000-0000-0000-0000-000000000001");

    // A later project change still reaches the fields the .meta leaves out.
    project.maxSize = 2048;
    const TextureMetaSettings after = Arcane::Editor::ReadTextureMetaSettingsDisplay(meta, project, &set);
    CHECK(after.maxSize == 2048u);
    CHECK_FALSE(after.srgb);
    CHECK(set.srgb);
    fs::remove_all(dir, ec);
}
