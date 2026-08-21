// THE SEVERANCE -- headless proof that the frame's DATA-SUPPLY side needs no
// graphics device.
//
// Three producers are exercised without one. Device-free is not a mode they are
// put into; it is the only shape they have.
//
//   * Batcher2D  -- Create() builds an instance whose
//     Begin/SetLayer/Quad*/Drain/RegisterMaterial/MaterialDesc are fully live
//     and whose End() records nothing and refuses loudly.
//   * SpriteMaterialCache -- a real dxc compile lands and Binds against a
//     device-less batcher: the registration carries the retained BLOBS, which
//     is what the graph recorder builds its own pipeline from.
//   * PostChainCache -- the same, and its PostChainDesc is published without
//     any chain object needing to exist: the cache never gated the desc publish
//     on one being possible.
//
// Why headless is the right home for this: everything above is CPU work over
// bytes. The half that genuinely needs a device -- that a device-CARRYING
// batcher records the same spans -- HAS NO COVERAGE AT ALL today. That is a
// named gap, not an oversight.
//
// The compiles here are REAL (in-process dxcompiler.dll, the same service the
// editor uses); ShaderCompilerTest.cpp proves that path is device-free, and
// the sprite/fullscreen templates come from the data/shaders directory the
// premake postbuild copies beside this exe.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Material/MaterialTypes.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/PostChainCache.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
#include <Arcane/Render/SpriteMaterialCache.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace Arcane;

namespace
{
    std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path d =
            std::filesystem::temp_directory_path() / "arcane_severance_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }

    // Two Guids that are stable within a case, so a span's identity can be
    // asserted by value rather than by "whatever came back".
    const Guid kTexA = *Guid::FromString("11111111-2222-3333-4444-555555555555");
    const Guid kTexB = *Guid::FromString("66666666-7777-8888-9999-aaaaaaaaaaaa");

    ParamDecl Decl(const char* name, MatParamType type)
    {
        ParamDecl d;
        d.name = name;
        d.type = type;
        return d;
    }

    // Drain the compile service until it goes idle. Mirrors ShaderCompilerTest's
    // DrainBlocking: the worker is a real thread, so a poll loop with a bound is
    // the honest wait.
    std::vector<ShaderCompileResult> DrainBlocking(ShaderCompiler& sc)
    {
        std::vector<ShaderCompileResult> all;
        for (int i = 0; i < 2000 && !sc.IsIdle(); ++i)
        {
            sc.Poll(0.0);
            for (ShaderCompileResult& r : sc.Drain())
                all.push_back(std::move(r));
            if (!sc.IsIdle())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (ShaderCompileResult& r : sc.Drain())
            all.push_back(std::move(r));
        return all;
    }
}

// =====================================================================
// Batcher2D -- the device-less instance
// =====================================================================

TEST_CASE("severance: Batcher2D::Create() yields a live, device-less batcher",
          "[batcher][severance]")
{
    auto batcher = Batcher2D::Create();
    REQUIRE(batcher != nullptr);   // NOT null: a missing device is not a failure

    batcher->Begin(128, 64);
    batcher->SetLayer(0, 0);
    batcher->QuadTextured(Batcher2D::kMaterialSprite, kTexA,
                          glm::vec2(0.0f), glm::vec2(16.0f),
                          glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
    batcher->QuadTextured(Batcher2D::kMaterialSprite, kTexB,
                          glm::vec2(16.0f, 0.0f), glm::vec2(16.0f),
                          glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
    batcher->Rect(glm::vec2(0.0f, 32.0f), glm::vec2(8.0f), glm::vec4(1.0f));

    const Batch2DDrained drained = batcher->Drain();

    // The geometry is fully there -- 3 quads, 4 vertices and 6 indices each.
    CHECK(drained.vertices.size() == 12);
    CHECK(drained.indices.size() == 18);
    CHECK(drained.viewport == glm::vec2(128.0f, 64.0f));
    CHECK(drained.globals != nullptr);

    // THE SPAN KEY. Three distinct texture identities (A, B, nil) -> three
    // runs. Without the Guid in the split they would coalesce into ONE run of
    // 18 indices and the graph recorder would draw one image three times. The
    // Guid IS the key -- a span carries no texture pointer to fall back on.
    REQUIRE(drained.spans.size() == 3);
    for (const Batch2DDrawSpan& span : drained.spans)
    {
        CHECK(span.material == Batcher2D::kMaterialSprite);
        CHECK(span.indexCount == 6);
    }
    CHECK(drained.spans[0].textureId == kTexA);
    CHECK(drained.spans[1].textureId == kTexB);
    CHECK(drained.spans[2].textureId.IsNil());   // Rect is untextured
    CHECK(drained.spans[0].firstIndex == 0);
    CHECK(drained.spans[1].firstIndex == 6);
    CHECK(drained.spans[2].firstIndex == 12);

    // Stats are the drained numbers (the HUD parity contract, Task 12).
    CHECK(batcher->Stats().quads == 3);
    CHECK(batcher->Stats().drawCalls == 3);
}

TEST_CASE("severance: two quads sharing one texture Guid still coalesce into one span",
          "[batcher][severance]")
{
    // The other half of the split rule: the Guid must SEPARATE distinct assets
    // without FRAGMENTING a shared one, or every textured sprite would be its
    // own draw call.
    auto batcher = Batcher2D::Create();
    REQUIRE(batcher != nullptr);

    batcher->Begin(64, 64);
    batcher->QuadTextured(Batcher2D::kMaterialSprite, kTexA,
                          glm::vec2(0.0f), glm::vec2(8.0f),
                          glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
    batcher->QuadTextured(Batcher2D::kMaterialSprite, kTexA,
                          glm::vec2(8.0f, 0.0f), glm::vec2(8.0f),
                          glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));

    const Batch2DDrained drained = batcher->Drain();
    REQUIRE(drained.spans.size() == 1);
    CHECK(drained.spans[0].textureId == kTexA);
    CHECK(drained.spans[0].indexCount == 12);
}

TEST_CASE("severance: Quad/Rect/Circle/Glyph record a NIL texture id", "[batcher][severance]")
{
    // The plain (pre-Task-2) submission API cannot name an asset, so every one
    // of its spans must read as untextured -- which is what makes the graph
    // recorder bind its white texel for them rather than a stale image.
    auto batcher = Batcher2D::Create();
    REQUIRE(batcher != nullptr);

    batcher->Begin(64, 64);
    batcher->Quad(glm::vec2(0.0f), glm::vec2(4.0f),
                  glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
    batcher->Circle(glm::vec2(32.0f), 4.0f, glm::vec4(1.0f));
    batcher->Glyph(glm::vec2(8.0f), glm::vec2(4.0f),
                   glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
    batcher->Line(glm::vec2(0.0f), glm::vec2(16.0f), 2.0f, glm::vec4(1.0f));
    batcher->Triangle(glm::vec2(0.0f), glm::vec2(4.0f, 0.0f), glm::vec2(0.0f, 4.0f),
                      glm::vec4(1.0f));

    const Batch2DDrained drained = batcher->Drain();
    REQUIRE_FALSE(drained.spans.empty());
    for (const Batch2DDrawSpan& span : drained.spans)
        CHECK(span.textureId.IsNil());
}

TEST_CASE("severance: a device-less End() records nothing and leaves the batch drainable",
          "[batcher][severance]")
{
    auto batcher = Batcher2D::Create();
    REQUIRE(batcher != nullptr);

    batcher->Begin(64, 64);
    batcher->Rect(glm::vec2(0.0f), glm::vec2(8.0f), glm::vec4(1.0f));

    // End() records nothing -- there is no device for it to touch. It must
    // still not silently claim to have drawn anything.
    batcher->End();
    CHECK(batcher->Stats().drawCalls == 0);
    CHECK(batcher->Stats().quads == 0);

    // ...and the batch it refused to record is still there for the graph
    // recorder, which is the whole point of refusing instead of clearing.
    const Batch2DDrained drained = batcher->Drain();
    CHECK(drained.spans.size() == 1);
    CHECK(drained.vertices.size() == 4);
}

TEST_CASE("severance: RegisterMaterial accepts a BYTES-ONLY registration with no device",
          "[batcher][severance]")
{
    auto batcher = Batcher2D::Create();
    REQUIRE(batcher != nullptr);

    auto templ = std::make_shared<MaterialTemplate>(MaterialTemplate::Build(
        "severance", 1, { Decl("Speed", MatParamType::Float),
                          Decl("Detail", MatParamType::Texture) }));
    auto instance = std::make_shared<MaterialInstance>(
        std::shared_ptr<const MaterialTemplate>(templ));

    Material2DDesc desc;
    desc.templ    = templ;
    desc.instance = instance;
    desc.vsBytes  = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{ 1, 2, 3, 4 });
    desc.psBytes  = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{ 5, 6, 7, 8 });

    const std::uint16_t id = batcher->RegisterMaterial(desc);
    REQUIRE(id != Batcher2D::kInvalidMaterialId);
    CHECK(id >= 3);   // 0..2 are the built-ins

    const Material2DDesc* published = batcher->MaterialDesc(id);
    REQUIRE(published != nullptr);
    REQUIRE(published->vsBytes != nullptr);   // the BYTES are the product
    REQUIRE(published->psBytes != nullptr);
    CHECK(published->vsBytes->size() == 4);
    CHECK(published->psBytes->size() == 4);
    CHECK(published->templ == templ);
    CHECK(published->instance == instance);
    // The declared-texture WIDTH the graph recorder's t1.. range needs comes
    // from the template, and the template is its only authority.
    CHECK(published->templ->TextureCount() == 1);

    // A registration with NO bytes is still refused: nothing could ever
    // record it.
    Material2DDesc empty;
    empty.templ    = templ;
    empty.instance = instance;
    CHECK(batcher->RegisterMaterial(empty) == Batcher2D::kInvalidMaterialId);
}

// =====================================================================
// The material caches -- bytes are the product when there is no device
// =====================================================================

TEST_CASE("severance: SpriteMaterialCache binds with a NULL device and publishes bytes",
          "[render][severance]")
{
    ShaderCompiler compiler;
    REQUIRE(compiler.Initialize(/*debounceSeconds=*/0.0));

    ShaderSourceProvider provider;
    provider.AddRoot("data/shaders");

    const auto dir = TempDir("sprite_material");
    MaterialAssetData data;
    data.id      = Guid::Generate();
    data.name    = "SeveranceSprite";
    data.kind    = "sprite";
    data.snippet = "float4 shade(Varyings v) { return v.color; }\n";
    REQUIRE(SaveMaterialAsset(dir / "mat.arcmat", data));

    SpriteMaterialCache::Services services;
    services.compiler = &compiler;
    services.sources  = &provider;
    services.assets   = nullptr;   // THE SEVERANCE
    services.backend  = GraphicsBackend::D3D12;
    services.resolveAsset = [&](const Guid& g) -> std::optional<std::filesystem::path>
    {
        return g == data.id ? std::optional(dir / "mat.arcmat") : std::nullopt;
    };
    SpriteMaterialCache cache(std::move(services));

    auto batcher = Batcher2D::Create();
    REQUIRE(batcher != nullptr);

    cache.Request(data.id, 0.0);
    for (const ShaderCompileResult& r : DrainBlocking(compiler))
        cache.ConsumeResult(r, *batcher);

    // It BOUND: a registration exists in the batcher's table...
    const auto& table = cache.Table();
    const auto entry = table.find(data.id);
    REQUIRE(entry != table.end());

    // ...and it carries the bytes the graph recorder needs.
    const Material2DDesc* desc = batcher->MaterialDesc(entry->second);
    REQUIRE(desc != nullptr);
    REQUIRE(desc->vsBytes != nullptr);
    REQUIRE(desc->psBytes != nullptr);
    CHECK_FALSE(desc->vsBytes->empty());
    CHECK_FALSE(desc->psBytes->empty());
    REQUIRE(desc->templ != nullptr);
    REQUIRE(desc->instance != nullptr);

    compiler.Shutdown();
}

TEST_CASE("severance: PostChainCache publishes a PostChainDesc with a NULL device",
          "[render][severance]")
{
    ShaderCompiler compiler;
    REQUIRE(compiler.Initialize(/*debounceSeconds=*/0.0));

    ShaderSourceProvider provider;
    provider.AddRoot("data/shaders");

    const auto dir = TempDir("post_chain");
    MaterialAssetData data;
    data.id      = Guid::Generate();
    data.name    = "SeverancePost";
    data.snippet =
        "float4 shade(Varyings v)\n"
        "{ return InputTexture.Sample(MaterialSampler, v.uv); }\n";
    REQUIRE(SaveMaterialAsset(dir / "post.arcmat", data));

    PostChainCache::Services services;
    services.compiler = &compiler;
    services.sources  = &provider;
    services.assets   = nullptr;   // THE SEVERANCE (see the sprite-material
                                   // case above for the deleted `device`)
    services.backend  = GraphicsBackend::D3D12;
    services.resolveAsset = [&](const Guid& g) -> std::optional<std::filesystem::path>
    {
        return g == data.id ? std::optional(dir / "post.arcmat") : std::nullopt;
    };
    PostChainCache cache(std::move(services));

    cache.Request(data.id, 0.0);
    for (const ShaderCompileResult& r : DrainBlocking(compiler))
        cache.ConsumeResult(r);

    // THE DESC IS THE PRODUCT: bytes, layout and values, which is the whole of
    // what the graph's PostChainNode consumes and the whole of what this cache
    // produces. There is no chain object, and no gate to sit behind.
    const PostChainDesc* desc = cache.Desc(data.id);
    REQUIRE(desc != nullptr);
    REQUIRE(desc->templ != nullptr);
    REQUIRE(desc->instance != nullptr);
    REQUIRE(desc->passes.size() == 1);
    REQUIRE(desc->passes[0].vsBytes != nullptr);
    REQUIRE(desc->passes[0].psBytes != nullptr);
    CHECK_FALSE(desc->passes[0].vsBytes->empty());
    CHECK_FALSE(desc->passes[0].psBytes->empty());
    CHECK(desc->chainInputSlots >= 1);
    // Instance() tracks the desc: both are published in the same breath.
    CHECK(cache.Instance(data.id) == desc->instance.get());

    compiler.Shutdown();
}
