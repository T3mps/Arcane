// Asset-manager arc (Plan 1 Task 12): ValidateCreateName -- the PURE half of
// the unified create dialog. The dialog's draw half is ImGui and is not
// driven here. Since T3-D4 the dialog's unit is compiled into the test exe
// (premake5.lua's ArcaneTests list) for one more pure function,
// MakeCreateDialogState, pinned at the bottom of this file.
//
// Fixture shape follows AssetPanelModelTest.cpp / AssetBrowserTest.cpp: a REAL
// temp directory with REAL files, so the uniqueness rule is exercised against
// the same std::filesystem the editor calls, not a fake.

#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetPanelModel.hpp"
#include "Panels/CreateAssetDialog.hpp"
#include "Project/ClassTemplates.hpp"

#include <Arcane/Project/AssetRegistry.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

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

TEST_CASE("Create C++ System options start at Fixed Update and Both",
          "[editor][create]")
{
    const CreateDialogState state;
    const CreateAssetResult result;
    CHECK(state.systemPhaseIndex == 0);
    CHECK(state.systemRoleIndex == 0);
    CHECK(result.systemPhaseIndex == 0);
    CHECK(result.systemRoleIndex == 0);

    const auto options = ClassTemplates::SystemOptionsForChoiceIndices(
        state.systemPhaseIndex, state.systemRoleIndex);
    CHECK(options.phase == Arcane::SystemPhase::FixedUpdate);
    CHECK(options.role == Arcane::RoleMask::Both);
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

// Assets -> Create -> C++ Class (the editor<->IDE surface, step 3): a sixth
// create kind whose files land under Source/, not Content/. The header is the
// kind's PRIMARY file (uniqueness validates against it; the .cpp is derived),
// and a Source rail row bridges to it exactly as Material's row bridges to
// Material.
TEST_CASE("Create-kind vocabulary: C++ Class lands under Source/ and bridges from AssetKind::Source", "[editor][create]")
{
    CHECK(kCreateAssetKindCount == 7);
    CHECK(CreateKindForAssetKind(AssetKind::Source) == CreateAssetKind::CppClass);

    CHECK(std::string(CreateKindTitle(CreateAssetKind::CppClass))         == "Create C++ Class");
    CHECK(std::string(CreateKindExtension(CreateAssetKind::CppClass))     == ".hpp");
    CHECK(std::string(CreateKindDefaultFolder(CreateAssetKind::CppClass)) == "");   // Source/ itself
    CHECK(std::string(CreateNounForExtension(".hpp"))                     == "class");

    // The manifest's sourceDir moves the DEFAULT folder, never the root: the
    // source:// mount stays Source/ (the browser shows every module), and the
    // Location combo pre-selects the game module's own directory. Same
    // string shape the model uses for folders (trailing slash, "" = root).
    CHECK(CppClassDefaultFolder("Source")        == "");
    CHECK(CppClassDefaultFolder("Source/Game")   == "Game/");
    CHECK(CppClassDefaultFolder("Source/Game/")  == "Game/");     // tolerant of a trailing slash
    CHECK(CppClassDefaultFolder("Source/a/b")    == "a/b/");

    // The per-kind ROOT directory under the project: every asset kind is
    // Content/, source is Source/. This is the one thing the dialog's
    // Location combo and the dispatcher's target path both key off.
    CHECK(std::string(CreateKindRoot(CreateAssetKind::CppClass)) == "Source");
    CHECK(std::string(CreateKindRoot(CreateAssetKind::Material)) == "Content");
    CHECK(std::string(CreateKindRoot(CreateAssetKind::Scene))    == "Content");
}

// F4 plan 1 Task 11 (spec s8): `Create > Mesh >` is a submenu of the five
// primitives, and each entry raises the ONE CreateAssetRequest with its
// MeshSource preset -- the request grows a field, the dialog and MintMeshAsset
// stay the single entry. There is no second creation path to test for; what
// this pins is the vocabulary the submenu, the dispatcher, and the scene's
// `Add > 3D Object` (which mints `Content/meshes/<Primitive>.arcmesh` by name)
// all read from, so a label, a file stem, and a MeshSource can never drift.
TEST_CASE("Create > Mesh > <primitive>: the request carries a MeshSource preset; "
          "the five primitives name their .arcmesh files", "[editor][create]")
{
    // Default: no preset -- the dialog keeps MeshAssetData's own Cube default.
    const CreateAssetRequest plain;
    CHECK(plain.kind == CreateAssetKind::Material);
    CHECK(plain.prefillMeshSource == -1);

    // A submenu entry: the SAME request type, kind Mesh, plus the preset. The
    // value is MeshSource's own persisted numbering (MeshAsset.hpp), never a
    // menu index -- Cube is 1, Plane is 0.
    CreateAssetRequest cube;
    cube.kind              = CreateAssetKind::Mesh;
    cube.prefillMeshSource = static_cast<int>(Arcane::MeshSource::Cube);
    CHECK(cube.kind == CreateAssetKind::Mesh);
    CHECK(cube.prefillMeshSource == 1);
    CHECK(static_cast<Arcane::MeshSource>(cube.prefillMeshSource) == Arcane::MeshSource::Cube);
    // The other request fields stay at their defaults -- a mesh preset does not
    // smuggle a parent/surface prefill along.
    CHECK_FALSE(cube.prefillParent.IsValid());
    CHECK(cube.prefillSurface == -1);

    // The roster, in menu order (spec s8: Cube / Plane / Sphere / Cylinder /
    // Capsule). Five generator sources; Imported is NOT a primitive.
    CHECK(kPrimitiveMeshSourceCount == 5);
    CHECK(kPrimitiveMeshSources[0] == Arcane::MeshSource::Cube);
    CHECK(kPrimitiveMeshSources[1] == Arcane::MeshSource::Plane);
    CHECK(kPrimitiveMeshSources[2] == Arcane::MeshSource::UvSphere);
    CHECK(kPrimitiveMeshSources[3] == Arcane::MeshSource::Cylinder);
    CHECK(kPrimitiveMeshSources[4] == Arcane::MeshSource::Capsule);

    // The name is the menu label, the file stem AND the spawned entity's
    // Identity name. UvSphere spells "Sphere" (the brief's file name).
    CHECK(std::string(PrimitiveMeshName(Arcane::MeshSource::Cube))     == "Cube");
    CHECK(std::string(PrimitiveMeshName(Arcane::MeshSource::Plane))    == "Plane");
    CHECK(std::string(PrimitiveMeshName(Arcane::MeshSource::UvSphere)) == "Sphere");
    CHECK(std::string(PrimitiveMeshName(Arcane::MeshSource::Cylinder)) == "Cylinder");
    CHECK(std::string(PrimitiveMeshName(Arcane::MeshSource::Capsule))  == "Capsule");
    CHECK(PrimitiveMeshName(Arcane::MeshSource::Imported) == nullptr);

    // The file, relative to Content/: the Mesh kind's own default folder
    // (CreateKindDefaultFolder -- ONE spelling of "meshes/", lowercase, the
    // folder ReferenceProject already carries) + the name + the kind's
    // extension. The registry keys off this path, so it must never come out
    // in two spellings.
    CHECK(PrimitiveMeshRelativePath(Arcane::MeshSource::UvSphere) == "meshes/Sphere.arcmesh");
    CHECK(PrimitiveMeshRelativePath(Arcane::MeshSource::Cube)     == "meshes/Cube.arcmesh");
    CHECK(PrimitiveMeshRelativePath(Arcane::MeshSource::Imported).empty());
}

TEST_CASE("Input Actions creation uses a native asset path and rejects name collisions", "[editor][create][input]")
{
    namespace fs = std::filesystem;
    CHECK(kCreateAssetKindCount == 7);
    CHECK(CreateKindForAssetKind(AssetKind::InputActions) == CreateAssetKind::InputActions);
    CHECK(std::string(CreateKindTitle(CreateAssetKind::InputActions)) == "Create Input Actions");
    CHECK(std::string(CreateKindExtension(CreateAssetKind::InputActions)) == ".arcinput");
    CHECK(std::string(CreateKindDefaultFolder(CreateAssetKind::InputActions)) == "input/");
    CHECK(std::string(CreateKindRoot(CreateAssetKind::InputActions)) == "Content");
    const auto dir = fs::temp_directory_path() /
        ("arcane_create_input_" + Arcane::Guid::Generate().ToString());
    fs::create_directories(dir);
    CHECK(ValidateCreateName("Player", dir, ".arcinput").ok);
    std::ofstream(dir / "Player.arcinput") << "{}";
    CHECK_FALSE(ValidateCreateName("Player", dir, ".arcinput").ok);
    std::error_code error;
    fs::remove_all(dir, error);
}

// T3-D4: MakeCreateDialogState -- the request -> the dialog's starting state,
// the whole of what EditorApp::BeginCreateAsset opens with (it adds only the
// C++ Class default folder). A Material Instance created FROM a material
// starts with that parent picked and Unreal's "<Parent>_Inst" name.
TEST_CASE("MakeCreateDialogState: an instance request from a material picks the parent and names it <parent>_Inst",
          "[editor][create]")
{
    const fs::path dir = FreshDir("make_state");
    std::ofstream(dir / "logo_showcase.arcmat", std::ios::binary)
        << R"({"id":"eeee0000-0000-4000-8000-000000000011","type":"material","kind":"sprite"})";
    std::ofstream(dir / "hero.png", std::ios::binary) << "not a real png";
    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 2);
    AssetPanelProviders providers;
    providers.refsFor = [](const Arcane::Guid&) -> std::optional<std::vector<Arcane::AssetRef>>
    { return std::vector<Arcane::AssetRef>{}; };
    providers.cookStateFor = [](const Arcane::Guid&) { return CookState::Cooked; };
    providers.surfaceFor = [](const Arcane::Guid&) -> std::optional<Arcane::MaterialSurface> { return std::nullopt; };
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    const Arcane::Guid parent = *Arcane::Guid::FromString("eeee0000-0000-4000-8000-000000000011");
    REQUIRE(model.Find(parent) != nullptr);
    Arcane::Guid texture;
    for (const auto& [guid, entry] : model.Entries())
        if (entry.kind == AssetKind::Texture)
            texture = guid;
    REQUIRE(texture.IsValid());

    SECTION("from a material")
    {
        const CreateDialogState st = MakeCreateDialogState({ CreateAssetKind::MaterialInstance, parent }, model, dir);
        CHECK(st.open);
        CHECK(st.parent == parent);
        CHECK_FALSE(st.pickerOpen);
        CHECK(std::string(st.name) == "logo_showcase_Inst");
        CHECK(st.request.prefillParent == parent);
    }
    SECTION("from empty space: no parent, the picker open, no name")
    {
        const CreateDialogState st = MakeCreateDialogState({ CreateAssetKind::MaterialInstance, {} }, model, dir);
        CHECK_FALSE(st.parent.IsValid());
        CHECK(st.pickerOpen);
        CHECK(st.name[0] == '\0');
    }
    SECTION("a parent the model does not know is kept but names nothing")
    {
        const Arcane::Guid stale = *Arcane::Guid::FromString("eeee0000-0000-4000-8000-0000000000ff");
        const CreateDialogState st = MakeCreateDialogState({ CreateAssetKind::MaterialInstance, stale }, model, dir);
        CHECK(st.parent == stale);
        CHECK(st.name[0] == '\0');
    }
    SECTION("a sprite's prefill lands in the texture field, never the parent, and names nothing")
    {
        const CreateDialogState st = MakeCreateDialogState({ CreateAssetKind::Sprite, texture }, model, dir);
        CHECK(st.texture == texture);
        CHECK_FALSE(st.parent.IsValid());
        CHECK_FALSE(st.pickerOpen);
        CHECK(st.name[0] == '\0');
    }
    SECTION("the surface prefill maps through the combo order; none is the default index")
    {
        CreateAssetRequest mesh{ CreateAssetKind::Material, {} };
        mesh.prefillSurface = static_cast<int>(Arcane::MaterialSurface::Mesh);
        CHECK(MakeCreateDialogState(mesh, model, dir).surface == MaterialSurfaceComboIndex(Arcane::MaterialSurface::Mesh));
        CHECK(MakeCreateDialogState({ CreateAssetKind::Material, {} }, model, dir).surface == kMaterialSurfaceDefaultIndex);
    }
}

// T3-D5: the default "<parent>_Inst" is free on disk or it is suffixed --
// "_Inst2", "_Inst3", ... -- in the folder the instance will land in (the
// parent's own), so the dialog never opens on "already exists".
TEST_CASE("MakeCreateDialogState: a colliding <parent>_Inst takes the next free _Inst<N> in the parent's folder",
          "[editor][create]")
{
    const fs::path root = FreshDir("make_state_unique");
    const fs::path folder = root / "Content" / "materials" / "sub";
    fs::create_directories(folder);
    std::ofstream(folder / "logo.arcmat", std::ios::binary)
        << R"({"id":"eeee0000-0000-4000-8000-000000000021","type":"material","kind":"sprite"})";
    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(root / "Content", "game") == 1);
    AssetPanelProviders providers;
    providers.refsFor = [](const Arcane::Guid&) -> std::optional<std::vector<Arcane::AssetRef>>
    { return std::vector<Arcane::AssetRef>{}; };
    providers.cookStateFor = [](const Arcane::Guid&) { return CookState::Cooked; };
    providers.surfaceFor = [](const Arcane::Guid&) -> std::optional<Arcane::MaterialSurface> { return std::nullopt; };
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    const Arcane::Guid parent = *Arcane::Guid::FromString("eeee0000-0000-4000-8000-000000000021");
    REQUIRE(model.Find(parent) != nullptr);
    const CreateAssetRequest request{ CreateAssetKind::MaterialInstance, parent };

    CreateDialogState st = MakeCreateDialogState(request, model, root);
    CHECK(st.defaultFolder == std::optional<std::string>("materials/sub"));
    CHECK(std::string(st.name) == "logo_Inst");

    std::ofstream(folder / "logo_Inst.arcmat", std::ios::binary) << "{}";
    st = MakeCreateDialogState(request, model, root);
    CHECK(std::string(st.name) == "logo_Inst2");
    CHECK(ValidateCreateName(st.name, folder, ".arcmat").ok);

    std::ofstream(folder / "logo_Inst2.arcmat", std::ios::binary) << "{}";
    CHECK(std::string(MakeCreateDialogState(request, model, root).name) == "logo_Inst3");

    // Only the parent's folder counts: the same name elsewhere collides with nothing.
    fs::create_directories(root / "Content" / "materials");
    std::ofstream(root / "Content" / "materials" / "logo_Inst3.arcmat", std::ios::binary) << "{}";
    CHECK(std::string(MakeCreateDialogState(request, model, root).name) == "logo_Inst3");
    std::error_code error;
    fs::remove_all(root, error);
}

TEST_CASE("ValidateRenameName: case-only rename accepted, real collision and bad characters refused", "[editor][create][assetops]")
{
    namespace fs = std::filesystem; const fs::path d = fs::temp_directory_path() / "arcane_rename_name_test";
    std::error_code ec; fs::remove_all(d, ec); fs::create_directories(d); std::ofstream(d / "a.png") << "x"; std::ofstream(d / "b.png") << "x";
    CHECK((ValidateRenameName("a", d / "a.png").ok && ValidateRenameName("A", d / "a.png").ok));   // no-op; case-only
    CHECK(ValidateRenameName("b", d / "a.png").message.find("already exists") != std::string::npos);
    CHECK(ValidateRenameName("a:b", d / "a.png").message.find("cannot contain") != std::string::npos);
    CHECK_FALSE(ValidateRenameName("a ", d / "a.png").ok);                                       // rule 1 fires before rule 3
    CHECK(ValidateRenameName("wall.png", d / "a.png").message.find("extension is fixed") != std::string::npos);   // s7.6: the extension is fixed; typing it must not yield wall.png.png
    CHECK(ValidateRenameName("wall.PNG", d / "a.png").message.find("extension is fixed") != std::string::npos);
    CHECK(ValidateRenameName("wall.jpg", d / "a.png").ok);                                       // only the file's OWN extension is refused
    fs::remove_all(d, ec);
}
