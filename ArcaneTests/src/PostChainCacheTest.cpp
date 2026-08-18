// PostChainCache headless coverage (post arc, slice 2): every refusal happens
// BEFORE a compile is submitted, so these run without a device or an
// initialized compiler -- the request simply never exposes a bound chain.
//
// NRI Phase 5a, Task 4: these cases read Desc() (was Chain()) -- this
// Fixture was already device-less (no Services::device set), so Chain()
// (the deleted NVRHI accessor) was always null here regardless; Desc() is
// the exact same "nothing bound" signal on the one path this cache has left.
// PostChainGpuTest.cpp, which pinned the successful end-to-end path (async
// compile, bind, last-good) via the NVRHI OffscreenCanvas/FullscreenMaterialChain
// harness, is deleted along with them -- see the commit body for that gap.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Render/PostChainCache.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <filesystem>
#include <optional>
#include <unordered_map>

using namespace Arcane;

namespace
{
    std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path d =
            std::filesystem::temp_directory_path() / "arcane_post_chain_cache_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }

    struct Fixture
    {
        // Constructed but never Initialize()d: refusals must not reach Submit.
        ShaderCompiler compiler;
        ShaderSourceProvider provider;
        std::unordered_map<Guid, std::filesystem::path> files;

        PostChainCache MakeCache()
        {
            provider.AddRoot("data/shaders");
            PostChainCache::Services s;
            s.compiler = &compiler;
            s.sources = &provider;
            s.resolveAsset = [this](const Guid& g)
                -> std::optional<std::filesystem::path>
            {
                const auto it = files.find(g);
                return it != files.end() ? std::optional(it->second) : std::nullopt;
            };
            return PostChainCache(std::move(s));
        }
    };
}

TEST_CASE("PostChainCache refuses pre-submit failure shapes", "[material]")
{
    Fixture fx;
    PostChainCache cache = fx.MakeCache();

    SECTION("an unresolved guid never exposes a chain")
    {
        const Guid ghost = Guid::Generate();
        cache.Request(ghost, 0.0);
        CHECK(cache.Desc(ghost) == nullptr);
        CHECK(cache.Instance(ghost) == nullptr);
        cache.Request(ghost, 1.0);   // failed is sticky until Invalidate
        CHECK(cache.Desc(ghost) == nullptr);
    }

    SECTION("a sprite material is refused -- the post slot is fullscreen only")
    {
        const auto dir = TempDir("sprite");
        MaterialAssetData data;
        data.id = Guid::Generate();
        data.name = "SpriteMat";
        data.kind = "sprite";
        data.snippet = "float4 shade(Varyings v) { return v.color; }\n";
        REQUIRE(SaveMaterialAsset(dir / "sprite.arcmat", data));
        fx.files[data.id] = dir / "sprite.arcmat";

        cache.Request(data.id, 0.0);
        CHECK(cache.Desc(data.id) == nullptr);
    }

    SECTION("a parent cycle is refused")
    {
        const auto dir = TempDir("cycle");
        MaterialAssetData a, b;
        a.id = Guid::Generate();
        b.id = Guid::Generate();
        a.parent = b.id;
        b.parent = a.id;
        a.name = "A";
        b.name = "B";
        REQUIRE(SaveMaterialAsset(dir / "a.arcmat", a));
        REQUIRE(SaveMaterialAsset(dir / "b.arcmat", b));
        fx.files[a.id] = dir / "a.arcmat";
        fx.files[b.id] = dir / "b.arcmat";

        cache.Request(a.id, 0.0);
        CHECK(cache.Desc(a.id) == nullptr);
    }

    SECTION("a DAG violation is a build error, refused before any compile")
    {
        const auto dir = TempDir("dag");
        MaterialAssetData data;
        data.id = Guid::Generate();
        data.name = "BadInputs";
        data.snippet =
            "float4 shade(Varyings v)\n"
            "{ return InputTexture.Sample(MaterialSampler, v.uv); }\n";
        data.baseInputs = { 3 };   // not the Scene sentinel, not an earlier pass
        REQUIRE(SaveMaterialAsset(dir / "bad.arcmat", data));
        fx.files[data.id] = dir / "bad.arcmat";

        cache.Request(data.id, 0.0);
        CHECK(cache.Desc(data.id) == nullptr);
    }

    SECTION("a foreign compile result is not consumed")
    {
        ShaderCompileResult r{};
        r.jobId = 12345;
        CHECK_FALSE(cache.ConsumeResult(r));
    }

    SECTION("Clear and Invalidate on unknown ids are safe no-ops")
    {
        cache.Invalidate(Guid::Generate());
        cache.Clear();
        CHECK(cache.Desc(Guid::Generate()) == nullptr);
    }
}
