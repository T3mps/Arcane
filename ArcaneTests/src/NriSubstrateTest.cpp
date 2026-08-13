// NRI substrate, Task 4: result-check discipline (ARC_NRI_CHECK/NriCheckImpl),
// the callback-to-latch wiring (MakeNriCallbacks), and the one-line identity
// log (LogNriIdentity). Headless -- [nri], inside the ~[gpu] dev gate.
//
// Include order matters in this file: NRI's Extensions/NRIDeviceCreation.h
// declares nri::Message with an enumerator literally named ERROR, and
// <windows.h> (dragged in transitively by Arcane/Render/Device.hpp -> spdlog)
// #defines ERROR via wingdi.h. Once that macro is live, every later textual
// "ERROR" in this translation unit -- including a qualified nri::Message::ERROR
// -- gets corrupted by preprocessor substitution. Keep the NRI includes first.
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriCommon.hpp>

#include <cstdint>
#include <vector>

TEST_CASE("nri: ARC_NRI_CHECK on SUCCESS returns true and does not bump RenderErrorCount", "[nri]")
{
    const uint64_t before = Arcane::RenderErrorCount();

    const bool ok = ARC_NRI_CHECK(nri::Result::SUCCESS);

    CHECK(ok);
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("nri: ARC_NRI_CHECK on a failure result returns false and bumps RenderErrorCount by exactly 1", "[nri]")
{
    // This is the one [nri] case that deliberately trips the REAL, shared
    // 0/0 gate latch (proving ARC_NRI_CHECK reaches it, not a fake local
    // counter) -- reset around it so this doesn't leak a permanent +1 into
    // every other test case's RenderErrorCount()==0 assertion for the rest
    // of the process (same "other cases must not leak into this one"
    // reasoning as GpuCrashReportTest.cpp's device-lost latch reset).
    Arcane::ResetRenderErrorCount();
    REQUIRE(Arcane::RenderErrorCount() == 0);

    const bool ok = ARC_NRI_CHECK(nri::Result::FAILURE);

    CHECK_FALSE(ok);
    CHECK(Arcane::RenderErrorCount() == 1);

    Arcane::ResetRenderErrorCount();
}

TEST_CASE("nri: NONE-backend device lifecycle via MakeNriCallbacks/LogNriIdentity leaves RenderErrorCount untouched", "[nri]")
{
    const uint64_t before = Arcane::RenderErrorCount();

    nri::DeviceCreationDesc desc{};
    desc.graphicsAPI = nri::GraphicsAPI::NONE;
    desc.callbackInterface = Arcane::MakeNriCallbacks();

    nri::Device* device = nullptr;
    // NONE-backend carve-out (task-4 brief): nriCreateDevice is the standard
    // creation path for every NRI backend, but the wrapper-path-only rule
    // forbids it for REAL backends (D3D12/VK), which must go through the
    // native-device wrapper extensions (nriCreateDeviceFrom*Device) instead.
    // NONE has no native device to wrap -- there is nothing to wrap it FROM
    // -- so nriCreateDevice is the only creation path that exists for it,
    // and is acceptable here, and only here.
    const nri::Result result = nriCreateDevice(desc, device);
    REQUIRE(result == nri::Result::SUCCESS);
    REQUIRE(device != nullptr);

    Arcane::LogNriIdentity(*device);

    nriDestroyDevice(device);

    CHECK(Arcane::RenderErrorCount() == before);
}

// ---------------------------------------------------------------------------
// Task 5: Graveyard -- fence-tagged deferred destruction. Pure-core cases
// first (no NRI device involved at all), then one NONE-device integration
// case proving Bury/Reap drives a real NRI CoreInterface function-table
// destroy call. Added here rather than a sibling file per the task-5 brief:
// this file stayed small (< 100 lines before this section) and Graveyard is
// the NRI substrate's next load-bearing piece, same as Task 4's cases.
// ---------------------------------------------------------------------------

TEST_CASE("graveyard: burial order is preserved for thunks buried at the same fence value", "[nri]")
{
    Arcane::Graveyard graveyard;
    std::vector<int> ran;

    graveyard.Bury(1, [&ran] { ran.push_back(1); });
    graveyard.Bury(1, [&ran] { ran.push_back(2); });
    graveyard.Bury(1, [&ran] { ran.push_back(3); });

    REQUIRE(graveyard.Pending() == 3);

    graveyard.Reap(1);

    CHECK(ran == std::vector<int>{1, 2, 3});
    CHECK(graveyard.Pending() == 0);
}

TEST_CASE("graveyard: Reap(completed) runs every thunk with fenceValue <= completed and leaves the rest", "[nri]")
{
    Arcane::Graveyard graveyard;
    std::vector<std::uint64_t> ran;

    graveyard.Bury(3, [&ran] { ran.push_back(3); });
    graveyard.Bury(5, [&ran] { ran.push_back(5); });
    graveyard.Bury(6, [&ran] { ran.push_back(6); });

    graveyard.Reap(5);

    // Not 6: fenceValue 6 has not completed yet at completedValue == 5.
    CHECK(ran == std::vector<std::uint64_t>{3, 5});
    CHECK(graveyard.Pending() == 1);

    graveyard.Reap(6);

    CHECK(ran == std::vector<std::uint64_t>{3, 5, 6});
    CHECK(graveyard.Pending() == 0);
}

TEST_CASE("graveyard: interleaved Bury/Reap calls run each thunk exactly once, in burial order", "[nri]")
{
    Arcane::Graveyard graveyard;
    std::vector<int> ran;

    graveyard.Bury(1, [&ran] { ran.push_back(1); });
    graveyard.Reap(1);
    CHECK(ran == std::vector<int>{1});

    graveyard.Bury(2, [&ran] { ran.push_back(2); });
    graveyard.Bury(2, [&ran] { ran.push_back(20); });

    // Reap at a value below both pending fence values: neither is due yet.
    graveyard.Reap(1);
    CHECK(ran == std::vector<int>{1});
    CHECK(graveyard.Pending() == 2);

    graveyard.Bury(4, [&ran] { ran.push_back(4); });

    // Reap between fence 2 and fence 4: the fence-2 pair is due, fence 4 is not.
    graveyard.Reap(3);
    CHECK(ran == std::vector<int>{1, 2, 20});
    CHECK(graveyard.Pending() == 1);

    graveyard.Reap(4);
    CHECK(ran == std::vector<int>{1, 2, 20, 4});
    CHECK(graveyard.Pending() == 0);
}

TEST_CASE("graveyard: Drain runs every pending thunk regardless of fence value", "[nri]")
{
    Arcane::Graveyard graveyard;
    std::vector<int> ran;

    graveyard.Bury(10, [&ran] { ran.push_back(10); });
    graveyard.Bury(20, [&ran] { ran.push_back(20); });
    graveyard.Bury(30, [&ran] { ran.push_back(30); });

    REQUIRE(graveyard.Pending() == 3);

    // None of these fence values have "completed" from any queue's
    // perspective -- Drain runs them anyway, which is exactly its contract
    // (teardown / device-loss path; caller has already idled the GPU).
    graveyard.Drain();

    CHECK(ran == std::vector<int>{10, 20, 30});
    CHECK(graveyard.Pending() == 0);
}

TEST_CASE("graveyard: Pending reflects the exact count of unreaped burials", "[nri]")
{
    Arcane::Graveyard graveyard;
    CHECK(graveyard.Pending() == 0);

    graveyard.Bury(1, [] {});
    CHECK(graveyard.Pending() == 1);

    graveyard.Bury(1, [] {});
    graveyard.Bury(2, [] {});
    CHECK(graveyard.Pending() == 3);

    graveyard.Reap(1);
    CHECK(graveyard.Pending() == 1);

    graveyard.Reap(2);
    CHECK(graveyard.Pending() == 0);
}

TEST_CASE("graveyard: an explicit Drain leaves it empty, so destruction takes the safe (nothing-to-do) path", "[nri]")
{
    // Exercises the same "must be empty/drained before destruction" contract
    // the destructor enforces (fatal ARC_ASSERT in debug; a WARN + Drain in
    // release -- see Graveyard.cpp) without needing to observe either of
    // those config-specific behaviors directly: Drain() is the exact
    // operation release-mode destruction performs, called here explicitly.
    std::vector<int> ran;
    {
        Arcane::Graveyard graveyard;
        graveyard.Bury(1, [&ran] { ran.push_back(1); });
        graveyard.Bury(2, [&ran] { ran.push_back(2); });

        graveyard.Drain();

        CHECK(ran == std::vector<int>{1, 2});
        CHECK(graveyard.Pending() == 0);

        // graveyard goes out of scope here with Pending() == 0: the
        // destructor's debug-mode ARC_ASSERT(m_graves.empty()) passes
        // silently, and its release-mode Drain() is a no-op.
    }
}

TEST_CASE("nri: Graveyard defers a CoreInterface DestroyBuffer through a NONE device to Reap, no latch growth", "[nri]")
{
    const uint64_t before = Arcane::RenderErrorCount();

    nri::DeviceCreationDesc desc{};
    desc.graphicsAPI = nri::GraphicsAPI::NONE;
    desc.callbackInterface = Arcane::MakeNriCallbacks();

    nri::Device* device = nullptr;
    // NONE-backend carve-out -- see the lifecycle test above for the full
    // rationale (nriCreateDevice is the only creation path NONE has).
    REQUIRE(ARC_NRI_CHECK(nriCreateDevice(desc, device)));
    REQUIRE(device != nullptr);

    nri::CoreInterface core{};
    REQUIRE(ARC_NRI_CHECK(nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core)));

    // NONE's CreateBuffer (ImplNONE.cpp) hands back a non-null sentinel
    // object rather than a real allocation, and DestroyBuffer is a no-op --
    // there is nothing to leak-check on this backend. What this case proves
    // is that Bury/Reap correctly threads a REAL NRI function-table call
    // (core.DestroyBuffer, resolved through nriGetInterface, same as
    // production code would call it) through to execution at the right
    // fence value, not a fake/local stand-in for "destroy".
    nri::BufferDesc bufferDesc{};
    bufferDesc.size = 256;

    nri::Buffer* buffer = nullptr;
    REQUIRE(ARC_NRI_CHECK(core.CreateBuffer(*device, bufferDesc, buffer)));
    REQUIRE(buffer != nullptr);

    Arcane::Graveyard graveyard;
    graveyard.Bury(42, [&core, buffer] { core.DestroyBuffer(buffer); });
    CHECK(graveyard.Pending() == 1);

    // NONE-backend footgun (brief): GetFenceValue is hard-wired to 0
    // (ImplNONE.cpp) -- there is nothing meaningful to read back, so this
    // feeds Reap the releasing value by hand rather than querying the fence.
    graveyard.Reap(42);
    CHECK(graveyard.Pending() == 0);

    nriDestroyDevice(device);

    CHECK(Arcane::RenderErrorCount() == before);
}
