// Asset-manager arc (Plan 1 Task 12): ValidateCreateName -- the PURE half of
// the unified create dialog. This is the ONLY thing this file tests: the
// dialog's other half is ImGui, and the test exe compiles no ImGui TU (see
// CreateAssetDialog.hpp's own header comment on the split, and premake5.lua's
// ArcaneTests file list, which source-compiles only the pure editor units).
//
// Fixture shape follows AssetPanelModelTest.cpp / AssetBrowserTest.cpp: a REAL
// temp directory with REAL files, so the uniqueness rule is exercised against
// the same std::filesystem the editor calls, not a fake.

#include <catch2/catch_test_macros.hpp>

#include "Panels/CreateAssetDialog.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace Arcane::Editor;
namespace fs = std::filesystem;

namespace
{
    // A fresh, empty directory per case (fresh-environment convention: never
    // mutate-and-restore a shared one).
    fs::path FreshDir(const char* leaf)
    {
        const fs::path dir = fs::temp_directory_path() / "arcane_create_asset_dialog_test" / leaf;
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        return dir;
    }

    void Touch(const fs::path& dir, const std::string& fileName)
    {
        std::ofstream(dir / fileName, std::ios::binary) << "{}";
    }
}

TEST_CASE("ValidateCreateName accepts an ordinary unused name", "[editor][create]")
{
    const fs::path dir = FreshDir("valid");

    const CreateNameCheck ok = ValidateCreateName("pulse_sprite", dir, ".arcmat");
    CHECK(ok.ok);
    CHECK(ok.message.empty());

    // Dots, dashes and spaces INSIDE the name are all legal -- only the
    // deny-set characters and leading/trailing dots/spaces are refused.
    CHECK(ValidateCreateName("my material v1.2", dir, ".arcmat").ok);
}

TEST_CASE("ValidateCreateName refuses an empty or blank name", "[editor][create]")
{
    const fs::path dir = FreshDir("empty");

    const CreateNameCheck empty = ValidateCreateName("", dir, ".arcmat");
    CHECK_FALSE(empty.ok);
    CHECK_FALSE(empty.message.empty());

    // All-whitespace is empty once trimmed of the leading/trailing spaces the
    // character rule already refuses -- it must not fall through as "valid".
    CHECK_FALSE(ValidateCreateName("   ", dir, ".arcmat").ok);
}

TEST_CASE("ValidateCreateName refuses the character deny-set and path separators",
          "[editor][create]")
{
    const fs::path dir = FreshDir("chars");

    // Rule 1's full deny-set, one case per character (spec s7 / UE
    // AssetViewUtils.cpp:1420-1509's own set).
    for (const char* bad : { "a\\b", "a/b", "a:b", "a*b", "a?b", "a\"b",
                             "a<b", "a>b", "a|b" })
    {
        const CreateNameCheck c = ValidateCreateName(bad, dir, ".arcmat");
        INFO("name = " << bad);
        CHECK_FALSE(c.ok);
        CHECK_FALSE(c.message.empty());
    }

    // Leading/trailing dots and spaces (Windows silently strips them, which
    // would mint a file under a name the user did not type).
    CHECK_FALSE(ValidateCreateName(".hidden", dir, ".arcmat").ok);
    CHECK_FALSE(ValidateCreateName("trailing.", dir, ".arcmat").ok);
    CHECK_FALSE(ValidateCreateName(" leading", dir, ".arcmat").ok);
    CHECK_FALSE(ValidateCreateName("trailing ", dir, ".arcmat").ok);
}

TEST_CASE("ValidateCreateName refuses a name whose full path exceeds the length budget",
          "[editor][create]")
{
    const fs::path dir = FreshDir("length");

    // 300 legal characters: over the 240-character absolute budget no matter
    // how short the temp dir happens to be on this machine.
    const std::string longName(300, 'a');
    const CreateNameCheck tooLong = ValidateCreateName(longName, dir, ".arcmat");
    CHECK_FALSE(tooLong.ok);
    CHECK_FALSE(tooLong.message.empty());

    // A short name in the same directory still passes -- the rule is about the
    // total, not about this directory being unusable.
    CHECK(ValidateCreateName("short", dir, ".arcmat").ok);
}

TEST_CASE("ValidateCreateName refuses a duplicate with a message DISTINCT from the char rule",
          "[editor][create]")
{
    const fs::path dir = FreshDir("unique");
    Touch(dir, "pulse_sprite.arcmat");

    const CreateNameCheck dup = ValidateCreateName("pulse_sprite", dir, ".arcmat");
    CHECK_FALSE(dup.ok);
    // The message must NAME the collision (spec s7 / the task brief: "a <kind>
    // named X already exists here") so the fix is obvious from the text, and
    // must not read like the illegal-character refusal.
    CHECK(dup.message.find("pulse_sprite") != std::string::npos);
    CHECK(dup.message.find("already exists") != std::string::npos);
    CHECK(dup.message != ValidateCreateName("a/b", dir, ".arcmat").message);

    // Uniqueness is per DIRECTORY and per EXTENSION: the same stem with a
    // different extension does not collide, and the same name in a different
    // directory does not either.
    CHECK(ValidateCreateName("pulse_sprite", dir, ".arcmesh").ok);
    CHECK(ValidateCreateName("pulse_sprite", FreshDir("unique_other"), ".arcmat").ok);
}

TEST_CASE("Create-kind vocabulary and the AssetKind bridge", "[editor][create]")
{
    // The two enums do NOT share a numbering -- this is the ONE sanctioned
    // bridge, and the bug it exists to prevent (the rail writing a raw
    // AssetKind int into a CreateAssetKind field) is exactly a numbering
    // mismatch, so pin the mapping by VALUE.
    CHECK(CreateKindForAssetKind(AssetKind::Material) == CreateAssetKind::Material);
    CHECK(CreateKindForAssetKind(AssetKind::Mesh)     == CreateAssetKind::Mesh);
    CHECK(CreateKindForAssetKind(AssetKind::Sprite)   == CreateAssetKind::Sprite);
    CHECK(CreateKindForAssetKind(AssetKind::Scene)    == CreateAssetKind::Scene);
    // Nothing else can be minted -- the six non-creatable kinds map to nullopt.
    CHECK_FALSE(CreateKindForAssetKind(AssetKind::Texture).has_value());
    CHECK_FALSE(CreateKindForAssetKind(AssetKind::Audio).has_value());
    CHECK_FALSE(CreateKindForAssetKind(AssetKind::Font).has_value());
    CHECK_FALSE(CreateKindForAssetKind(AssetKind::Data).has_value());
    CHECK_FALSE(CreateKindForAssetKind(AssetKind::Diagnostic).has_value());
    CHECK_FALSE(CreateKindForAssetKind(AssetKind::Other).has_value());
    // AssetKind::Sprite is 6, CreateAssetKind::Sprite is 3 -- the raw-int
    // reinterpretation the bridge replaces would have been wrong.
    CHECK(static_cast<int>(AssetKind::Sprite) != static_cast<int>(CreateAssetKind::Sprite));

    CHECK(std::string(CreateKindExtension(CreateAssetKind::Material))         == ".arcmat");
    CHECK(std::string(CreateKindExtension(CreateAssetKind::MaterialInstance)) == ".arcmat");
    CHECK(std::string(CreateKindExtension(CreateAssetKind::Mesh))             == ".arcmesh");
    CHECK(std::string(CreateKindExtension(CreateAssetKind::Sprite))           == ".arcsprite");
    CHECK(std::string(CreateKindExtension(CreateAssetKind::Scene))            == ".arcscene");

    CHECK(std::string(CreateKindDefaultFolder(CreateAssetKind::Material)) == "materials/");
    CHECK(std::string(CreateKindTitle(CreateAssetKind::MaterialInstance))
          == "Create Material Instance");
    CHECK(std::string(CreateNounForExtension(".arcmat")) == "material");
}
