// SceneRenderResolverTest.cpp -- the sprite-resolution lift (2026-07-29). Two
// groups, both CPU-only (no device, no compiler, no batcher):
//
//   [1] SpriteCache semantics, pinned because they MOVED. The cache spent the
//       whole sprite arc in ArcaneEditor/src/ and nothing tested it; lifting it
//       into Arcane.dll is only safe if the behaviour that made it work in the
//       editor is nailed down -- resolve-once, failure memoised, invalidation
//       forcing a re-read.
//
//   [2] SceneRenderResolver PUBLISHES. This is the group that would have failed
//       before the arc: ArcaneRuntime never called SetSpriteTable, so no scene
//       could draw a textured sprite there whatever it contained. The assertion
//       is deliberately made through the REGISTRY RESOURCE the submission path
//       reads (SpriteTable::Resolve), not through the cache's own Table(),
//       because "the cache resolved it" was never the broken half.
//
// Registry-touching tests bind to Arcane::Test::SharedTypeContext() -- never a
// bare Arcane::Runtime, which would steal Arcane.dll's TypeContext slot.

#include <Arcane/Assets/Assets.hpp>          // Arcane::Assets (the EvictingAssets fake), PixelData
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Host/SceneRenderResolver.hpp>
#include <Arcane/Project/AssetId.hpp>       // AssetId::FromGuid -- driving the fake's eviction by hand
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/SpriteCache.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include <catch2/catch_test_macros.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    fs::path MakeTempDir(const char* tag)
    {
        std::error_code ec;
        fs::path d = fs::temp_directory_path() / (std::string("arcane_resolver_") + tag);
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        return d;
    }

    // A sprite asset on disk with a known pivot. No texture Guid: this suite runs
    // with no Assets facade behind it, and ComputeSpriteGeom's documented
    // no-texture fallback (1x1 m, full UVs) is what an untextured resolve yields
    // either way -- so the pivot is the field that proves the JSON was READ
    // rather than a default-constructed entry being handed back.
    Arcane::Guid WriteSprite(const fs::path& file, glm::vec2 pivot)
    {
        Arcane::SpriteAssetData data;
        data.id    = Arcane::Guid::Generate();
        data.name  = "probe";
        data.pivot = pivot;
        REQUIRE(Arcane::SaveSpriteAsset(file, data));
        return data.id;
    }

    // As above but WITH a source texture Guid, so SpriteCache takes the
    // textured branch (PixelsFor) instead of the untextured one.
    Arcane::Guid WriteTexturedSprite(const fs::path& file, const Arcane::Guid& texture,
                                     float ppu)
    {
        Arcane::SpriteAssetData data;
        data.id      = Arcane::Guid::Generate();
        data.name    = "probe";
        data.texture = texture;
        data.ppu     = ppu;
        REQUIRE(Arcane::SaveSpriteAsset(file, data));
        return data.id;
    }

    // An Assets facade whose PIXEL ENTRY IS EVICTED BY A SECOND CALL -- the
    // production behaviour, modelled deterministically.
    //
    // In the real facade PixelsFor hands back a bare pointer INTO its
    // LRU-budgeted pixel cache and releases its own pin before returning
    // (Assets.cpp: `m_pixels.Acquire(key); EnforceBudget(); m_pixels.Release(key);`),
    // and any other inserting call ends `Put(...); EnforceBudget();` -- a sweep
    // that evicts the globally least-recently-used entry across ALL the
    // facade's caches, the pixel cache included. So a caller holding the
    // pointer across a second Assets call can be reading freed memory.
    //
    // GetBytes is the second call here: an inserting, sweeping call on the
    // same shared LRU clock, which is the only property this fake needs from
    // it. What the fake pins is the ORDER: it hands out a stable pointer, then
    // on the second call RESETS the object it pointed at to a default-
    // constructed PixelData (0x0, no bytes) -- the deterministic stand-in for
    // "that buffer was freed and its memory reused". Read before the call, the
    // dimensions are the image's; read after, they are zeros and
    // ComputeSpriteGeom silently returns its 1x1 m fallback.
    //
    // WHY A FAKE AND NOT THE REAL FACADE: reaching a real cross-cache eviction
    // needs a budget small enough to sweep, and ReferenceProject's marker PNG
    // is 179 bytes against a 256 MiB default -- so the sweep never fires in any
    // gate. The fake makes the ordering hazard reachable in ~[gpu].
    class EvictingAssets final : public Arcane::Assets
    {
    public:
        EvictingAssets(std::uint32_t w, std::uint32_t h)
        {
            m_pixels.width  = w;
            m_pixels.height = h;
            m_pixels.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0xFF);
        }

        void SetContentRoot(const std::filesystem::path&) override {}
        void SetAssetResolver(AssetResolver) override {}

        const Arcane::PixelData* PixelsFor(const Arcane::Guid&) override
        {
            ++pixelsForCalls;
            return &m_pixels;
        }

        // THE EVICTING CALL -- see the class comment. Everything else answers
        // null without touching m_pixels.
        std::shared_ptr<const std::vector<std::uint8_t>> GetBytes(const Arcane::AssetId&) override
        {
            ++evictingCalls;
            m_pixels = Arcane::PixelData{};   // "the LRU sweep took the pixel entry"
            return nullptr;
        }

        std::shared_ptr<const std::vector<std::uint8_t>> GetBytes(const std::filesystem::path&) override { return nullptr; }
        std::shared_ptr<const nlohmann::json> GetJson(const std::filesystem::path&) override { return nullptr; }
        std::shared_ptr<const nlohmann::json> GetJson(const Arcane::AssetId&) override { return nullptr; }
        Arcane::AssetStats Stats() const override { return {}; }

        int pixelsForCalls = 0;
        int evictingCalls  = 0;

    private:
        Arcane::PixelData m_pixels;
    };
}

// ---------------------------------------------------------------- [1] the cache

TEST_CASE("SpriteCache resolves a Guid once and keeps serving that entry", "[sprite]")
{
    const fs::path dir = MakeTempDir("resolve_once");
    const fs::path file = dir / "probe.arcsprite";
    const Arcane::Guid id = WriteSprite(file, {0.25f, 0.75f});

    int resolveCalls = 0;
    Arcane::SpriteCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return file;
    };
    Arcane::SpriteCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().size() == 1);
    REQUIRE(resolveCalls == 1);
    const Arcane::SpriteEntry first = cache.Table().at(id);
    CHECK(first.pivot.x == 0.25f);
    CHECK(first.pivot.y == 0.75f);
    // (A `CHECK(first.texture == nullptr)` stood here -- no Assets facade, so
    // untextured -- until ABI v15 deleted SpriteEntry::texture.)
    CHECK(first.sizeMeters.x == 1.0f);        // ComputeSpriteGeom's 0x0 fallback
    CHECK(first.sizeMeters.y == 1.0f);

    // Per-frame sweeps call Request for every referenced Guid every frame; the
    // whole point is that this is free after the first one.
    for (int i = 0; i < 5; ++i)
        cache.Request(id);
    CHECK(resolveCalls == 1);

    // A nil Guid is not an error and must not create an entry (SpriteRenderer's
    // default is nil -- an entity with no sprite assigned).
    cache.Request(Arcane::Guid::Nil());
    CHECK(cache.Table().size() == 1);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("SpriteCache memoises a failed resolve as the visible placeholder", "[sprite]")
{
    const Arcane::Guid id = Arcane::Guid::Generate();

    int resolveCalls = 0;
    Arcane::SpriteCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return std::nullopt;   // not in the asset registry
    };
    Arcane::SpriteCache cache(std::move(s));

    cache.Request(id);
    // The failure lands IN the published table as a default entry, unlike
    // SpriteMaterialCache which keeps failures out of its table. A sprite has no
    // implicit fallback -- the component exists to draw one specific asset -- so a
    // broken one must still be VISIBLE as the 1x1 m placeholder instead of
    // silently vanishing.
    REQUIRE(cache.Table().contains(id));
    const Arcane::SpriteEntry entry = cache.Table().at(id);
    // (A `CHECK(entry.texture == nullptr)` stood here until ABI v15 deleted
    // SpriteEntry::texture.)
    CHECK(entry.sizeMeters.x == 1.0f);
    CHECK(entry.sizeMeters.y == 1.0f);

    // Memoised: a per-frame sweep must not re-hit the filesystem for a Guid
    // already known to be unresolvable.
    cache.Request(id);
    cache.Request(id);
    CHECK(resolveCalls == 1);
}

TEST_CASE("SpriteCache::Invalidate forces the next Request to re-read the file", "[sprite]")
{
    const fs::path dir = MakeTempDir("invalidate");
    const fs::path file = dir / "probe.arcsprite";
    const Arcane::Guid id = WriteSprite(file, {0.0f, 0.0f});

    Arcane::SpriteCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path> { return file; };
    Arcane::SpriteCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().at(id).pivot.x == 0.0f);

    // The sprite editor re-saves the asset with a new pivot. Without the
    // invalidation the viewport keeps drawing the PRE-edit geometry forever --
    // Request is a once-per-Guid cache, so this hook IS the mechanism that makes
    // an edit show up.
    Arcane::SpriteAssetData edited;
    edited.id    = id;
    edited.name  = "probe";
    edited.pivot = {1.0f, 0.5f};
    REQUIRE(Arcane::SaveSpriteAsset(file, edited));

    cache.Request(id);
    CHECK(cache.Table().at(id).pivot.x == 0.0f);   // still stale: cached

    cache.Invalidate(id);
    CHECK_FALSE(cache.Table().contains(id));
    cache.Request(id);
    CHECK(cache.Table().at(id).pivot.x == 1.0f);
    CHECK(cache.Table().at(id).pivot.y == 0.5f);

    // Clear is the project-switch path: a Guid resolves through the CURRENT
    // project's registry, so nothing may survive the switch.
    cache.Clear();
    CHECK(cache.Table().empty());

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("SpriteCache makes exactly ONE Assets call, so nothing can evict PixelsFor's pointer",
          "[sprite][pixels]")
{
    // THE HAZARD IS ABSENT RATHER THAN AVOIDED, and THAT is what this case
    // pins. Assets.hpp is explicit that a PixelsFor pointer may not be held
    // across another Assets call: it is owned by an LRU-budgeted cache, and
    // "callers that need it to outlive the current call must copy it".
    // SpriteCache::Request makes EXACTLY ONE Assets call, so no interleaved
    // eviction is reachable at all. Re-add a second Assets call of any name and
    // this fails, pointing at the paragraph above.
    //
    // Residency is NriTextureCache's job and a sprite is named by Guid alone,
    // which is why one call is enough.
    //
    // Invisible to every gate before this case: ReferenceProject's marker PNG
    // is 179 bytes against a 256 MiB budget, so the sweep never fires there.
    const fs::path dir  = MakeTempDir("pixels_order");
    const fs::path file = dir / "probe.arcsprite";
    const Arcane::Guid texture = Arcane::Guid::Generate();
    const Arcane::Guid id = WriteTexturedSprite(file, texture, /*ppu=*/100.0f);

    EvictingAssets assets(/*w=*/8, /*h=*/4);

    Arcane::SpriteCache::Services s;
    s.assets = &assets;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path> { return file; };
    Arcane::SpriteCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(assets.pixelsForCalls == 1);
    REQUIRE(assets.evictingCalls == 0);   // THE INVARIANT: no second call to evict across
    REQUIRE(cache.Table().contains(id));

    // 8x4 px at 100 px/m -- the dimensions really were read. Had an eviction
    // interleaved, they would be 0x0 and ComputeSpriteGeom would return its
    // documented 1x1 m / full-UV fallback: the silent wrong-size sprite this
    // whole case exists to prevent.
    const Arcane::SpriteEntry entry = cache.Table().at(id);
    CHECK(entry.sizeMeters.x == 0.08f);
    CHECK(entry.sizeMeters.y == 0.04f);
    // Residency is not this cache's business -- the record names its image by
    // Guid and carries no device object. (A `CHECK(entry.texture == nullptr)`
    // stood beside this until ABI v15 deleted the field itself, which makes
    // the same statement structurally.)
    CHECK(entry.textureId == texture);

    // ...and EvictingAssets still MODELS the eviction, so the guard above is
    // pinning a real hazard rather than a fake that quietly stopped evicting.
    // Driven by hand, since SpriteCache no longer drives it.
    assets.GetBytes(Arcane::AssetId::FromGuid(texture));
    const Arcane::PixelData* after = assets.PixelsFor(texture);
    REQUIRE(after != nullptr);
    CHECK(after->width == 0);
    CHECK(after->height == 0);

    std::error_code ec; fs::remove_all(dir, ec);
}

// ------------------------------------------------------------- [2] the resolver

TEST_CASE("SceneRenderResolver publishes the scene's SpriteTable into the registry",
          "[sprite][host]")
{
    const fs::path dir = MakeTempDir("publish");
    REQUIRE(Arcane::Project::Create(dir / "Game", "G").has_value());
    const fs::path file = dir / "Game" / "Content" / "probe.arcsprite";
    const Arcane::Guid id = WriteSprite(file, {0.125f, 0.875f});

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);
    REQUIRE(rt.OpenProject(dir / "Game") == true);
    // Registered so the resolver's project-backed asset resolver can find it by
    // Guid -- the same registration the editor performs when it mints an asset.
    REQUIRE(rt.RegisterCreatedAsset(file).has_value());

    const Astra::Entity e = rt.Registry().CreateEntity();
    Arcane::SpriteRenderer sr;
    sr.sprite = id;
    rt.Registry().AddComponent<Arcane::SpriteRenderer>(e, sr);

    // No device, no batcher, no compiler: sprite resolution has no compile step,
    // and that independence is load-bearing (gating it on the compiler once made
    // every sprite on a DXC-less machine draw as an untextured 1x1 quad).
    Arcane::SceneRenderResolver::Services rs;
    rs.runtime = &rt;
    Arcane::SceneRenderResolver resolver(std::move(rs));

    // BEFORE: nothing published. This is precisely the pre-arc state of the
    // standalone host -- the resource is absent, so RenderSubmissionSystem
    // resolves every sprite to null and draws the 1x1 m untextured tint quad.
    const Arcane::SpriteTable* before = rt.Registry().GetResource<Arcane::SpriteTable>();
    CHECK((before == nullptr || before->sprites == nullptr));

    Arcane::SceneRenderResolver::FrameInfo frame;
    frame.now            = 0.0;
    frame.dt             = 1.0 / 60.0;
    frame.viewportWidth  = 1280.0f;
    frame.viewportHeight = 720.0f;
    resolver.Refresh(frame);

    // AFTER: the submission path can resolve the scene's sprite Guid.
    const Arcane::SpriteTable* table = rt.Registry().GetResource<Arcane::SpriteTable>();
    REQUIRE(table != nullptr);
    REQUIRE(table->sprites != nullptr);
    const Arcane::SpriteEntry* entry = table->Resolve(id);
    REQUIRE(entry != nullptr);
    CHECK(entry->pivot.x == 0.125f);
    CHECK(entry->pivot.y == 0.875f);

    // The material table publishes too (empty here -- the sprite carries no
    // material -- but present, so Resolve returns the plain-pipeline 0 rather
    // than reading a null resource).
    const Arcane::SpriteMaterialTable* materials =
        rt.Registry().GetResource<Arcane::SpriteMaterialTable>();
    REQUIRE(materials != nullptr);
    REQUIRE(materials->materials != nullptr);
    CHECK(materials->Resolve(Arcane::Guid::Generate()) == 0);

    // The frame's material globals ride along (the standalone host never set
    // these before either, so an animated material read zeros).
    CHECK(resolver.Globals().viewportWidth == 1280.0f);
    CHECK(resolver.Globals().viewportHeight == 720.0f);

    // No PostProcess entity in the scene -> no chain, which is the plain
    // canvas->tonemap path rather than a broken one. PostDesc() is the
    // bytes-only description the graph recorder reads.
    CHECK(resolver.PostInstance() == nullptr);
    CHECK(resolver.PostDesc() == nullptr);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("SceneRenderResolver picks up a sprite added after the first Refresh",
          "[sprite][host]")
{
    const fs::path dir = MakeTempDir("late_add");
    REQUIRE(Arcane::Project::Create(dir / "Game", "G").has_value());
    const fs::path file = dir / "Game" / "Content" / "late.arcsprite";
    const Arcane::Guid id = WriteSprite(file, {0.5f, 0.5f});

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);
    REQUIRE(rt.OpenProject(dir / "Game") == true);
    REQUIRE(rt.RegisterCreatedAsset(file).has_value());

    Arcane::SceneRenderResolver::Services rs;
    rs.runtime = &rt;
    Arcane::SceneRenderResolver resolver(std::move(rs));

    Arcane::SceneRenderResolver::FrameInfo frame;
    frame.dt = 1.0 / 60.0;
    resolver.Refresh(frame);   // empty scene: publishes empty tables

    // A scene loaded (or an entity spawned) after the host's first frame is the
    // normal case -- the host boots, THEN opens a scene. Resolution is a
    // per-frame sweep for exactly this reason.
    const Astra::Entity e = rt.Registry().CreateEntity();
    Arcane::SpriteRenderer sr;
    sr.sprite = id;
    rt.Registry().AddComponent<Arcane::SpriteRenderer>(e, sr);

    frame.now = 1.0 / 60.0;
    resolver.Refresh(frame);

    const Arcane::SpriteTable* table = rt.Registry().GetResource<Arcane::SpriteTable>();
    REQUIRE(table != nullptr);
    REQUIRE(table->Resolve(id) != nullptr);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("SceneRenderResolver un-publishes its tables when it dies", "[sprite][host]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);
    {
        Arcane::SceneRenderResolver::Services rs;
        rs.runtime = &rt;
        Arcane::SceneRenderResolver resolver(std::move(rs));
        Arcane::SceneRenderResolver::FrameInfo frame;
        resolver.Refresh(frame);
        // Two steps, not one chained deref: an absent resource must fail this
        // test, not crash the whole suite (it did, when the red-check
        // deliberately disabled the publish).
        const Arcane::SpriteTable* live = rt.Registry().GetResource<Arcane::SpriteTable>();
        REQUIRE(live != nullptr);
        REQUIRE(live->sprites != nullptr);
    }
    // The registry's tables are NON-OWNING pointers into the resolver's maps, so
    // a dead resolver must leave nulls behind rather than dangling pointers --
    // the reason every host declares it to destruct before the Runtime.
    const Arcane::SpriteTable* table = rt.Registry().GetResource<Arcane::SpriteTable>();
    REQUIRE(table != nullptr);
    CHECK(table->sprites == nullptr);
    const Arcane::SpriteMaterialTable* materials =
        rt.Registry().GetResource<Arcane::SpriteMaterialTable>();
    REQUIRE(materials != nullptr);
    CHECK(materials->materials == nullptr);
}
