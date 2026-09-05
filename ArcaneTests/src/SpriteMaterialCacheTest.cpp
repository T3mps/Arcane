// SpriteMaterialCacheTest.cpp -- the missing kind gate (desk diagnosis
// .superpowers/sdd/2026-09-04-f2b-asset-cook-and-bindless/desk-diag-
// spritecache.md). SpriteMaterialCache::Request() is async (submits a real
// DXC compile through ShaderCompiler), so this file follows
// PostChainCacheTest.cpp's own convention: refusals are pinned HEADLESS,
// with an uninitialized ShaderCompiler and no device -- Submit() short-
// circuits (`!m_available`) before anything device-shaped is touched, so a
// refusal never needs to reach that point anyway.
//
// Table()/Desc()-style accessors stay empty either way in headless mode
// (nothing is ever bound without a device), so they cannot by themselves
// prove the KIND GATE fired rather than nothing happening at all -- the
// log-capture idiom (mirrored from SerializationNegativeTest.cpp/
// MaterialGraphTest.cpp) is what actually pins the refusal and its wording.
//
// The full successful bind (real dxc compile, device-less Batcher2D,
// Table() populated) is already pinned end-to-end by SeveranceTest.cpp's
// "SpriteMaterialCache binds with a NULL device..." case, which already
// sets kind="sprite" -- that is this gate's existing positive control and
// is not duplicated here.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
#include <Arcane/Render/SpriteMaterialCache.hpp>

#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>

using namespace Arcane;

namespace
{
    namespace fs = std::filesystem;

    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_sprite_material_cache_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        return d;
    }

    // Log-capture idiom, mirrored from SerializationNegativeTest.cpp:211-222
    // (and MaterialGraphTest.cpp's own copy): attach a callback sink to the
    // engine logger so a WARN this suite fires ON PURPOSE is asserted on
    // instead of dumped into the gate's output as noise. `out` holds the
    // LAST record captured.
    std::shared_ptr<spdlog::sinks::callback_sink_mt> AttachLogCapture(std::string& out)
    {
        auto cb = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&out](const spdlog::details::log_msg& m) { out.assign(m.payload.data(), m.payload.size()); });
        Log::Engine()->sinks().push_back(cb);
        return cb;
    }
    void DetachLogCapture(const std::shared_ptr<spdlog::sinks::callback_sink_mt>& cb)
    {
        auto& sinks = Log::Engine()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), cb), sinks.end());
    }

    // A resolvable BASE material of the given kind, no snippet needed -- the
    // wrong-kind cases must be refused before the snippet is ever read.
    Guid WriteKindedMaterial(const fs::path& file, const char* kind)
    {
        MaterialAssetData data;
        data.id   = Guid::Generate();
        data.name = "probe-wrong-kind";
        data.kind = kind;
        REQUIRE(SaveMaterialAsset(file, data));
        return data.id;
    }

    // An INSTANCE of `parent` -- no kind of its own (MaterialAsset.hpp:5-9:
    // both come from the base at the end of the parent chain). This is the
    // case a leaf-only check would wave through.
    Guid WriteMaterialInstance(const fs::path& file, const Guid& parent)
    {
        MaterialAssetData data;
        data.id     = Guid::Generate();
        data.name   = "probe-instance";
        data.parent = parent;
        REQUIRE(SaveMaterialAsset(file, data));
        return data.id;
    }

    struct Fixture
    {
        // Constructed but never Initialize()d: refusals must not reach
        // Submit -- same discipline as PostChainCacheTest.cpp's Fixture.
        ShaderCompiler compiler;
        ShaderSourceProvider provider;
        std::unordered_map<Guid, fs::path> files;
        int resolveCalls = 0;

        SpriteMaterialCache MakeCache()
        {
            provider.AddRoot("data/shaders");
            SpriteMaterialCache::Services s;
            s.compiler = &compiler;
            s.sources  = &provider;
            s.resolveAsset = [this](const Guid& g) -> std::optional<fs::path>
            {
                ++resolveCalls;
                const auto it = files.find(g);
                return it != files.end() ? std::optional(it->second) : std::nullopt;
            };
            return SpriteMaterialCache(std::move(s));
        }
    };
}

TEST_CASE("SpriteMaterialCache refuses a material whose chain base is not \"sprite\"-kind",
          "[sprite][material]")
{
    // The authoring mistake the desk diagnosis names: a SpriteRenderer's
    // material field pointed at a perfectly valid mesh or fullscreen
    // .arcmat (F2a's reference_mesh.arcmat / the post arc's
    // reference_post.arcmat, reachable through the kind-blind Inspector
    // picker -- a separate, larger fix, not this one). It used to reach a
    // real async DXC submit and only fail once the drain came back, logging
    // the misleading "failed to compile" -- this refusal happens
    // synchronously, inside Request(), before any compile is even
    // considered.
    Fixture fx;
    SpriteMaterialCache cache = fx.MakeCache();
    const fs::path dir = TempDir("wrong_kind");

    const Guid meshId       = WriteKindedMaterial(dir / "mesh.arcmat", "mesh");
    const Guid fullscreenId = WriteKindedMaterial(dir / "fullscreen.arcmat", "fullscreen");
    // An instance of the mesh base: the gate reads the chain's BASE, not
    // the leaf, because an instance carries no kind of its own.
    const Guid instId = WriteMaterialInstance(dir / "inst.arcmat", meshId);
    fx.files = { { meshId, dir / "mesh.arcmat" },
                 { fullscreenId, dir / "fullscreen.arcmat" },
                 { instId, dir / "inst.arcmat" } };

    std::string captured;
    auto cb = AttachLogCapture(captured);
    cache.Request(meshId, 0.0);
    DetachLogCapture(cb);

    CHECK_FALSE(cache.Table().contains(meshId));
    // The diagnostic must say what is ACTUALLY wrong -- a wrong-kind
    // material on a sprite slot -- not "failed to compile": name the kind
    // found and what the slot wants.
    CHECK(captured.find("not a sprite material") != std::string::npos);
    CHECK(captured.find("'mesh'") != std::string::npos);
    CHECK(captured.find("failed to compile") == std::string::npos);

    cache.Request(fullscreenId, 0.0);
    CHECK_FALSE(cache.Table().contains(fullscreenId));

    captured.clear();
    cb = AttachLogCapture(captured);
    cache.Request(instId, 0.0);
    DetachLogCapture(cb);
    CHECK_FALSE(cache.Table().contains(instId));
    // Same refusal, reading the INSTANCE's base kind ("mesh"), not the
    // instance's own (nonexistent) kind.
    CHECK(captured.find("not a sprite material") != std::string::npos);
    CHECK(captured.find("'mesh'") != std::string::npos);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("SpriteMaterialCache's wrong-kind refusal is memoized, not re-warned per frame",
          "[sprite][material]")
{
    Fixture fx;
    SpriteMaterialCache cache = fx.MakeCache();
    const fs::path dir = TempDir("memo");
    const Guid meshId = WriteKindedMaterial(dir / "mesh.arcmat", "mesh");
    fx.files = { { meshId, dir / "mesh.arcmat" } };

    cache.Request(meshId, 0.0);
    CHECK(fx.resolveCalls == 1);

    // Watched: a second and third Request for the SAME frame-driven Guid
    // must not re-resolve the asset or re-fire the WARN -- `failed` already
    // memoizes it, same as every other Request() refusal in this cache.
    std::string captured;
    auto cb = AttachLogCapture(captured);
    cache.Request(meshId, 1.0);
    cache.Request(meshId, 2.0);
    DetachLogCapture(cb);

    CHECK(fx.resolveCalls == 1);
    CHECK(captured.empty());

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("SpriteMaterialCache does not refuse a legitimate sprite-kind material",
          "[sprite][material]")
{
    // The control that keeps the two refusal cases above from being
    // vacuous: the SAME cache, the same call shape, a "sprite"-kind base --
    // passes the gate and reaches the real compile submission (no fail()
    // lambda runs, so no WARN of any kind fires). The full end-to-end bind
    // -- real dxc compile, device-less Batcher2D, Table() populated -- is
    // already pinned by SeveranceTest.cpp's "SpriteMaterialCache binds with
    // a NULL device..." case (kind="sprite" there too); this is the
    // headless half PostChainCacheTest.cpp's own coverage-gap note
    // describes -- refusals (and their absence) only, no device.
    Fixture fx;
    SpriteMaterialCache cache = fx.MakeCache();
    const fs::path dir = TempDir("legit");
    MaterialAssetData data;
    data.id      = Guid::Generate();
    data.name    = "probe-sprite";
    data.kind    = "sprite";
    data.snippet = "float4 shade(Varyings v) { return v.color; }\n";
    REQUIRE(SaveMaterialAsset(dir / "sprite.arcmat", data));
    fx.files = { { data.id, dir / "sprite.arcmat" } };

    std::string captured;
    auto cb = AttachLogCapture(captured);
    cache.Request(data.id, 0.0);
    DetachLogCapture(cb);

    CHECK(captured.empty());   // no fail() lambda ran -- the gate let it through

    std::error_code ec; fs::remove_all(dir, ec);
}
