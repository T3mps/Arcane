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
#include <Arcane/Render/Nri/NriCommon.hpp>

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
