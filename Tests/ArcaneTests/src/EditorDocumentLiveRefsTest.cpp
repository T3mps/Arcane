// T5 s7.5: EditorDocument::LiveReferences -- a dirty document's in-memory
// outgoing references, read from its typed data, feed the Delete planner's
// referencer union. The contract: for each document kind it equals what
// Assets::ListAssetReferences reports for the same data saved to disk.

#include <catch2/catch_test_macros.hpp>

#include "Documents/MeshDocument.hpp"
#include "Documents/ShaderEditorDocument.hpp"
#include "Documents/SpriteDocument.hpp"

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include <algorithm>

using namespace Arcane::Editor; namespace fs = std::filesystem;
namespace
{
    std::vector<Arcane::Guid> Sorted(std::vector<Arcane::Guid> v) { std::sort(v.begin(), v.end()); return v; }
    std::vector<Arcane::Guid> OnDisk(const Arcane::Guid& id, const fs::path& file)   // AssetReferencesTest.cpp's resolver pattern
    {
        auto as = Arcane::Assets::Create();
        as->SetAssetResolver([&](const Arcane::AssetId& a) -> std::optional<fs::path> { return a.Value() == id ? std::optional<fs::path>(file) : std::nullopt; });
        std::vector<Arcane::Guid> out; if (const auto r = as->ListAssetReferences(id)) for (const auto& x : *r) out.push_back(x.target);
        return Sorted(out);
    }
}
TEST_CASE("LiveReferences equals ListAssetReferences of the same data saved, per document kind", "[editor][assetops]")
{
    const fs::path d = fs::temp_directory_path() / "arcane_liverefs_test"; std::error_code ec; fs::remove_all(d, ec); fs::create_directories(d);
    Arcane::SpriteAssetData spr; spr.id = Arcane::Guid::Generate(); spr.texture = Arcane::Guid::Generate();
    SpriteDocument sd(SpriteDocument::Services{}, d / "s.arcsprite", spr); REQUIRE(sd.Save());
    CHECK(Sorted(sd.LiveReferences()) == OnDisk(spr.id, d / "s.arcsprite"));
    Arcane::MeshAssetData mesh; mesh.id = Arcane::Guid::Generate(); mesh.source = Arcane::MeshSource::Imported; mesh.importedSource = Arcane::Guid::Generate();
    mesh.slots.resize(2); mesh.slots[0].material = Arcane::Guid::Generate(); mesh.slots[1].material = Arcane::Guid::Generate();
    MeshDocument md(MeshDocument::Services{}, d / "m.arcmesh", mesh); REQUIRE(md.Save());
    CHECK(Sorted(md.LiveReferences()) == OnDisk(mesh.id, d / "m.arcmesh"));
    Arcane::MaterialAssetData mat; mat.id = Arcane::Guid::Generate(); mat.parent = Arcane::Guid::Generate();
    mat.params.emplace_back("albedo", Arcane::MatParamValue::MakeTexture(Arcane::Guid::Generate()));
    mat.params.emplace_back("tint", Arcane::MatParamValue::MakeFloat(1.0f));
    REQUIRE(Arcane::SaveMaterialAsset(d / "i.arcmat", mat));
    ShaderEditorDocument doc(DocServices{}, d / "i.arcmat", *Arcane::LoadMaterialAsset(d / "i.arcmat"));
    CHECK(Sorted(doc.LiveReferences()) == OnDisk(mat.id, d / "i.arcmat"));
    fs::remove_all(d, ec);
}
