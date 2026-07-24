// Material core (Slice 1 of the shader-editor arc): param primitives, template
// layout under HLSL cbuffer packing rules, instance override chains, the PackCB
// resolve-then-memcpy loop, the parallel texture table, and dirty serials.
// Pure CPU -- no GPU, no compile service.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Guid.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Material/MaterialTypes.hpp>

#include <cstring>
#include <memory>
#include <vector>

using namespace Arcane;

namespace
{
    ParamDecl Decl(const char* name, MatParamType type, MatParamValue def = {})
    {
        ParamDecl d;
        d.name = name;
        d.type = type;
        d.def = def;
        return d;
    }

    float ReadFloat(const std::vector<std::uint8_t>& blob, std::uint32_t offset)
    {
        float f = 0.0f;
        std::memcpy(&f, blob.data() + offset, sizeof(f));
        return f;
    }
}

TEST_CASE("HashParamName is stable, case-sensitive, constexpr", "[material]")
{
    constexpr std::uint32_t tint = HashParamName("Tint");
    static_assert(tint != 0);

    CHECK(HashParamName("Tint") == tint);
    CHECK(HashParamName("tint") != tint);
    CHECK(HashParamName("Tint2") != tint);
    CHECK(HashParamName("") == 2166136261u);   // FNV-1a offset basis
}

TEST_CASE("MatParamValue factories zero unused lanes; equality is type-aware", "[material]")
{
    const MatParamValue f = MatParamValue::MakeFloat(3.5f);
    CHECK(f.type == MatParamType::Float);
    CHECK(f.f[0] == 3.5f);
    CHECK(f.f[1] == 0.0f);
    CHECK(f.f[3] == 0.0f);

    const MatParamValue c = MatParamValue::MakeColor(1.0f, 0.5f, 0.25f);
    CHECK(c.type == MatParamType::Color);
    CHECK(c.f[3] == 1.0f);   // alpha defaults to 1

    // Same bits, different type -> not equal.
    MatParamValue f4 = MatParamValue::MakeFloat4(1.0f, 0.5f, 0.25f, 1.0f);
    CHECK(f4 != c);
    f4.type = MatParamType::Color;
    CHECK(f4 == c);

    const Guid g = Guid::Generate();
    CHECK(MatParamValue::MakeTexture(g) == MatParamValue::MakeTexture(g));
    CHECK(MatParamValue::MakeTexture(g) != MatParamValue::MakeTexture(Guid::Generate()));
}

TEST_CASE("MaterialTemplate::Build packs the CB under HLSL rules", "[material]")
{
    SECTION("floats pack tightly; vectors never straddle a 16-byte register")
    {
        const auto t = MaterialTemplate::Build("test", 1, {
            Decl("A", MatParamType::Float),    // 0
            Decl("B", MatParamType::Float2),   // 4  (4+8 <= 16: fits)
            Decl("C", MatParamType::Float),    // 12
            Decl("D", MatParamType::Float4),   // 16 (register start)
            Decl("E", MatParamType::Float),    // 32
            Decl("T", MatParamType::Texture),  // no CB slot, texture ordinal 0
            Decl("F", MatParamType::Color),    // 48 (36 would straddle)
        });

        CHECK(t.Find(HashParamName("A"))->cbOffset == 0);
        CHECK(t.Find(HashParamName("B"))->cbOffset == 4);
        CHECK(t.Find(HashParamName("C"))->cbOffset == 12);
        CHECK(t.Find(HashParamName("D"))->cbOffset == 16);
        CHECK(t.Find(HashParamName("E"))->cbOffset == 32);
        CHECK(t.Find(HashParamName("F"))->cbOffset == 48);
        CHECK(t.CbSize() == 64);

        const ParamDecl* tex = t.Find(HashParamName("T"));
        CHECK(tex->cbOffset == ParamDecl::kNoSlot);
        CHECK(tex->textureIndex == 0);
        CHECK(t.TextureCount() == 1);
    }

    SECTION("a float2 that would straddle bumps to the next register")
    {
        const auto t = MaterialTemplate::Build("test", 1, {
            Decl("A", MatParamType::Float),    // 0
            Decl("B", MatParamType::Float),    // 4
            Decl("C", MatParamType::Float),    // 8
            Decl("D", MatParamType::Float2),   // 12 would cross 16 -> 16
        });
        CHECK(t.Find(HashParamName("D"))->cbOffset == 16);
        CHECK(t.CbSize() == 32);
    }

    SECTION("float2 ending exactly on the register boundary does not bump")
    {
        const auto t = MaterialTemplate::Build("test", 1, {
            Decl("A", MatParamType::Float2),   // 0
            Decl("B", MatParamType::Float2),   // 8 (8+8 == 16: legal)
        });
        CHECK(t.Find(HashParamName("B"))->cbOffset == 8);
        CHECK(t.CbSize() == 16);
    }

    SECTION("no numeric params -> zero-size CB")
    {
        const auto empty = MaterialTemplate::Build("test", 1, {});
        CHECK(empty.CbSize() == 0);
        CHECK(empty.Defaults().empty());

        const auto texOnly = MaterialTemplate::Build("test", 1, {
            Decl("T", MatParamType::Texture),
        });
        CHECK(texOnly.CbSize() == 0);
        CHECK(texOnly.TextureCount() == 1);
    }

    SECTION("cbSize rounds up to a whole register")
    {
        const auto t = MaterialTemplate::Build("test", 1, {
            Decl("A", MatParamType::Float),
        });
        CHECK(t.CbSize() == 16);
    }
}

TEST_CASE("MaterialTemplate defaults blob carries defaults at their offsets", "[material]")
{
    const auto t = MaterialTemplate::Build("test", 1, {
        Decl("Speed", MatParamType::Float, MatParamValue::MakeFloat(2.0f)),
        Decl("Tint", MatParamType::Color, MatParamValue::MakeColor(1.0f, 0.5f, 0.25f, 0.75f)),
    });

    REQUIRE(t.Defaults().size() == t.CbSize());
    CHECK(ReadFloat(t.Defaults(), t.Find(HashParamName("Speed"))->cbOffset) == 2.0f);

    const std::uint32_t tintOff = t.Find(HashParamName("Tint"))->cbOffset;
    CHECK(ReadFloat(t.Defaults(), tintOff + 0) == 1.0f);
    CHECK(ReadFloat(t.Defaults(), tintOff + 4) == 0.5f);
    CHECK(ReadFloat(t.Defaults(), tintOff + 8) == 0.25f);
    CHECK(ReadFloat(t.Defaults(), tintOff + 12) == 0.75f);

    // Padding between Speed (4 bytes) and Tint stays zeroed.
    CHECK(ReadFloat(t.Defaults(), 4) == 0.0f);
}

TEST_CASE("MaterialTemplate drops duplicate names, first wins", "[material]")
{
    const auto t = MaterialTemplate::Build("test", 1, {
        Decl("Speed", MatParamType::Float, MatParamValue::MakeFloat(1.0f)),
        Decl("Speed", MatParamType::Float, MatParamValue::MakeFloat(9.0f)),
    });

    REQUIRE(t.Params().size() == 1);
    CHECK(t.Params()[0].def.f[0] == 1.0f);
    CHECK(t.CbSize() == 16);
}

TEST_CASE("MaterialInstance resolves override -> parent -> default", "[material]")
{
    const auto t = std::make_shared<MaterialTemplate>(MaterialTemplate::Build("test", 1, {
        Decl("Speed", MatParamType::Float, MatParamValue::MakeFloat(1.0f)),
        Decl("Tint", MatParamType::Color, MatParamValue::MakeColor(1.0f, 1.0f, 1.0f)),
    }));

    auto root = std::make_shared<MaterialInstance>(t);
    MatParamValue v;

    SECTION("template default shows through an empty instance")
    {
        REQUIRE(root->GetParam("Speed", v));
        CHECK(v.f[0] == 1.0f);
        CHECK_FALSE(root->HasOverride("Speed"));
    }

    SECTION("own override wins; ClearOverride restores the default")
    {
        REQUIRE(root->SetFloat("Speed", 3.0f));
        REQUIRE(root->GetParam("Speed", v));
        CHECK(v.f[0] == 3.0f);

        CHECK(root->ClearOverride("Speed"));
        REQUIRE(root->GetParam("Speed", v));
        CHECK(v.f[0] == 1.0f);
        CHECK_FALSE(root->ClearOverride("Speed"));   // nothing left to clear
    }

    SECTION("child chain: child override > parent override > default")
    {
        REQUIRE(root->SetFloat("Speed", 3.0f));
        MaterialInstance child(root);
        CHECK(&child.Template() == t.get());

        REQUIRE(child.GetParam("Speed", v));
        CHECK(v.f[0] == 3.0f);   // parent's override shows through

        REQUIRE(child.SetFloat("Speed", 5.0f));
        REQUIRE(child.GetParam("Speed", v));
        CHECK(v.f[0] == 5.0f);   // own override wins

        child.ClearOverride("Speed");
        REQUIRE(child.GetParam("Speed", v));
        CHECK(v.f[0] == 3.0f);   // back to the parent's value

        REQUIRE(child.GetParam("Tint", v));
        CHECK(v.f[0] == 1.0f);   // untouched param resolves to the default
    }

    SECTION("unknown names and type mismatches are rejected")
    {
        CHECK_FALSE(root->SetFloat("NoSuch", 1.0f));
        CHECK_FALSE(root->SetFloat("Tint", 1.0f));            // Color declared, Float given
        CHECK_FALSE(root->SetFloat4("Tint", 1, 1, 1, 1));     // Float4 != Color, strict
        CHECK_FALSE(root->GetParam("NoSuch", v));
        CHECK(root->OverrideCount() == 0);
    }
}

TEST_CASE("MaterialInstance::PackCB writes resolved values at their offsets", "[material]")
{
    const auto t = std::make_shared<MaterialTemplate>(MaterialTemplate::Build("test", 1, {
        Decl("Speed", MatParamType::Float, MatParamValue::MakeFloat(1.0f)),
        Decl("Tint", MatParamType::Color, MatParamValue::MakeColor(1.0f, 1.0f, 1.0f, 1.0f)),
        Decl("Noise", MatParamType::Texture),
    }));
    const std::uint32_t speedOff = t->Find(HashParamName("Speed"))->cbOffset;
    const std::uint32_t tintOff = t->Find(HashParamName("Tint"))->cbOffset;

    auto root = std::make_shared<MaterialInstance>(t);
    REQUIRE(root->SetColor("Tint", 0.25f, 0.5f, 0.75f, 1.0f));

    std::vector<std::uint8_t> cb(t->CbSize(), 0xCD);
    root->PackCB(cb.data(), cb.size());
    CHECK(ReadFloat(cb, speedOff) == 1.0f);    // default
    CHECK(ReadFloat(cb, tintOff + 0) == 0.25f);  // override
    CHECK(ReadFloat(cb, tintOff + 8) == 0.75f);

    // A child instance packs values resolved through the whole chain.
    MaterialInstance child(root);
    REQUIRE(child.SetFloat("Speed", 4.0f));
    child.PackCB(cb.data(), cb.size());
    CHECK(ReadFloat(cb, speedOff) == 4.0f);      // own override
    CHECK(ReadFloat(cb, tintOff + 1 * 4) == 0.5f);   // parent's override

    // Undersized destination: untouched.
    std::vector<std::uint8_t> small(t->CbSize() - 1, 0xCD);
    child.PackCB(small.data(), small.size());
    CHECK(small[0] == 0xCD);
}

TEST_CASE("MaterialInstance texture table resolves through the chain", "[material]")
{
    const Guid defaultTex = Guid::Generate();
    const auto t = std::make_shared<MaterialTemplate>(MaterialTemplate::Build("test", 1, {
        Decl("Albedo", MatParamType::Texture, MatParamValue::MakeTexture(defaultTex)),
        Decl("Noise", MatParamType::Texture),   // nil default
    }));

    auto root = std::make_shared<MaterialInstance>(t);

    auto tex = root->ResolveTextures();
    REQUIRE(tex.size() == 2);
    CHECK(tex[0] == defaultTex);
    CHECK(tex[1] == Guid::Nil());

    const Guid noiseTex = Guid::Generate();
    REQUIRE(root->SetTexture("Noise", noiseTex));
    MaterialInstance child(root);
    tex = child.ResolveTextures();
    CHECK(tex[0] == defaultTex);
    CHECK(tex[1] == noiseTex);
}

TEST_CASE("EffectiveSerial folds the parent chain", "[material]")
{
    const auto t = std::make_shared<MaterialTemplate>(MaterialTemplate::Build("test", 1, {
        Decl("Speed", MatParamType::Float, MatParamValue::MakeFloat(1.0f)),
    }));

    auto root = std::make_shared<MaterialInstance>(t);
    MaterialInstance child(root);

    const std::uint64_t s0 = child.EffectiveSerial();

    // Rejected writes and redundant same-value writes do not bump.
    CHECK_FALSE(root->SetFloat("NoSuch", 1.0f));
    CHECK(child.EffectiveSerial() == s0);
    REQUIRE(root->SetFloat("Speed", 2.0f));
    const std::uint64_t s1 = child.EffectiveSerial();
    CHECK(s1 > s0);   // parent edit is visible in the child's stamp
    REQUIRE(root->SetFloat("Speed", 2.0f));
    CHECK(child.EffectiveSerial() == s1);

    // Own edits and clears bump too.
    REQUIRE(child.SetFloat("Speed", 3.0f));
    const std::uint64_t s2 = child.EffectiveSerial();
    CHECK(s2 > s1);
    CHECK(child.ClearOverride("Speed"));
    CHECK(child.EffectiveSerial() > s2);
}

TEST_CASE("GlobalParams stays one cbuffer register", "[material]")
{
    STATIC_CHECK(sizeof(GlobalParams) == 16);
    CHECK(kMaterialCbSlot == 0);
    CHECK(kGlobalCbSlot == 1);

    const GlobalParams g;
    CHECK(g.time == 0.0f);
    CHECK(g.deltaTime == 0.0f);
}
