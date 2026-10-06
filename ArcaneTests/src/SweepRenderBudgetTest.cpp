// Settings arc S6-18: the render budgets -- the upload ring, the Batch2D and
// post-chain pools, the GPU scene's rows, the mesh residency budget and the
// ImGui pool chain -- are Restart cvars. Each node latches its caps once, at
// creation, and every default is the literal it replaces.

// NRI headers first (NriCommon.hpp's include-order rule).
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/ImGui/ImGuiNri.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/RenderBudgetSettings.hpp>

#include <imgui.h>

#include <cstdint>

using namespace Arcane;

TEST_CASE("sweep: render budget defaults are the pre-sweep literals", "[sweep][render-budget]")
{
    CHECK(RenderSettings{}.uploadRingBytesPerFrame == 4ull * 1024 * 1024);
    CHECK(RenderBatch2dSettings{}.maxMaterialSlots == 8u);
    CHECK(RenderBatch2dSettings{}.maxMaterialTextures == 8u);
    CHECK(RenderBatch2dSettings{}.maxSpriteTextures == 64u);
    CHECK(RenderBatch2dSettings{}.materialCbBytes == 256u);
    CHECK(RenderGpuSceneSettings{}.initialRows == 256u);
    CHECK(RenderGpuSceneSettings{}.scratchRowsPerFrame == 64u);
    CHECK(RenderMeshSettings{}.residencyBudgetBytes == 512ull * 1024 * 1024);
    CHECK(RenderPostSettings{}.maxPasses == 8u);
    CHECK(RenderPostSettings{}.maxTextures == 8u);
    CHECK(RenderPostSettings{}.materialCbBytes == 256u);
    CHECK(RenderImguiSettings{}.firstPoolSets == 64u);
    CHECK(RenderImguiSettings{}.maxPoolSetsPerLink == 1024u);
    Test::RequireDefault("render.batch2d.maxSpriteTextures", CVarValue::UInt32(64u));
    Test::RequireDefault("render.uploadRingBytesPerFrame", CVarValue::UInt64(4ull << 20));
}

TEST_CASE("sweep: a material CB region size is rounded down to a multiple of 256", "[sweep][render-budget]")
{
    STATIC_REQUIRE(MaterialCbRegionBytes(256u) == 256u);
    STATIC_REQUIRE(MaterialCbRegionBytes(511u) == 256u);
    STATIC_REQUIRE(MaterialCbRegionBytes(512u) == 512u);
    STATIC_REQUIRE(MaterialCbRegionBytes(65536u) == 65536u);
    STATIC_REQUIRE(MaterialCbRegionBytes(0u) == 256u);   // never below one region, even outside the Range
}

TEST_CASE("sweep: the ImGui pool chain and the mesh cache latch their budgets at creation (Restart)",
          "[sweep][render-budget]")
{
    // Device-less (the NONE backend), so this runs without a GPU: the pool
    // chain's bookkeeping and the cache's budget are CPU state.
    CVarRegistry& reg = CVarRegistry::Get();
    const auto set = [&reg](std::string_view name, const CVarValue& value)
    {
        REQUIRE(reg.Set(reg.Find(name), value, SetBy::Code) == SetResult::Applied);
        reg.PublishImmediate();
    };
    set("render.imgui.firstPoolSets", CVarValue::UInt32(16u));
    set("render.mesh.residencyBudgetBytes", CVarValue::UInt64(128ull << 20));

    auto device = NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    ImGuiContext* const previous = ImGui::GetCurrentContext();
    ImGuiContext* const context  = ImGui::CreateContext();
    REQUIRE(context != nullptr);

    Graveyard lane;
    {
        NriPipelineCache pipelines;
        pipelines.Bind(*device);
        const std::uint8_t vsBytes[4] = { 1, 2, 3, 4 };
        const std::uint8_t psBytes[4] = { 5, 6, 7, 8 };
        ImGuiNri backend;
        REQUIRE(backend.Init(*device, pipelines, vsBytes, psBytes));
        auto cache = NriMeshBufferCache::Create(*device);
        REQUIRE(cache != nullptr);

        // The published values, not the defaults.
        CHECK(backend.LinkCapacity(0) == 16u);
        CHECK(backend.LinkCapacity(1) == 32u);
        CHECK(cache->Budget() == (128ull << 20));

        // A later publish never resizes what already exists.
        set("render.imgui.firstPoolSets", CVarValue::UInt32(256u));
        set("render.mesh.residencyBudgetBytes", CVarValue::UInt64(1ull << 30));
        CHECK(backend.LinkCapacity(0) == 16u);
        CHECK(cache->Budget() == (128ull << 20));

        backend.Release(lane, 1);
        cache->Release(lane, 1);
        pipelines.Clear(lane, 1);
        lane.Drain();
    }

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
    reg.RevertLayer(SetBy::Code);
    reg.PublishImmediate();
}
