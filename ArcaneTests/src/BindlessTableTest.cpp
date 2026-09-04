// BindlessTable (F2b Task 9): the descriptor array + slot allocator behind
// bindless SRV indexing. CPU-only ([bindless]~[gpu]) -- runs entirely on the
// NONE backend through NriDevice::CreateNoneForTests(), the same sanctioned
// device-less carve-out NriTextureCacheTest/NriSubstrateTest already use.
//
// ImplNONE (ThirdParty/NRI/Source/NONE/ImplNONE.cpp) answers every Create*
// call with a dummy-but-non-null handle and every Destroy* call is a
// complete no-op that never even touches its argument -- so a fabricated
// nri::Descriptor* (any nonzero pointer value, never dereferenced) is safe
// wherever a test only needs Add()'s bookkeeping and never runs a burial
// through it. The Release() case below uses REAL descriptors (via
// core.CreateSampler, the cheapest Create* that hands back a
// nri::Descriptor*) because it actually reaps the burial and wants to prove
// that succeeds cleanly, not just that nothing crashes on a fake handle.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp
// (Extensions/NRIDeviceCreation.h, pulled in by NriDevice.hpp, declares
// nri::Message::ERROR, and <windows.h> -- dragged in transitively by
// Arcane/Base/Log.hpp -> spdlog -- #defines ERROR via wingdi.h. NriDevice.hpp
// must therefore land before anything that reaches Log.hpp, exactly as
// NriTextureCacheTest.cpp orders it: the NRI/Arcane-Nri headers first, THEN
// RenderErrorLatch.hpp (which is what actually pulls Log.hpp in this TU).
#include <NRI.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Nri/BindlessTable.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>

#include <spdlog/sinks/callback_sink.h>
#include <Arcane/Base/Log.hpp>

#undef ERROR

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // Log-capture idiom, mirrored from MaterialGraphTest.cpp/
    // SerializationNegativeTest.cpp's AttachLogCapture: attach a callback
    // sink to the engine logger so a WARN this suite fires ON PURPOSE is
    // asserted on directly. Extended with a call COUNT (those two files only
    // ever needed the last message) -- the whole point of the capacity-
    // exhaustion case below is proving the warn fires exactly ONCE, not
    // once per refused Add().
    std::shared_ptr<spdlog::sinks::callback_sink_mt> AttachLogCapture(std::string& out, int& count)
    {
        auto cb = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&out, &count](const spdlog::details::log_msg& m)
            {
                out.assign(m.payload.data(), m.payload.size());
                ++count;
            });
        Arcane::Log::Engine()->sinks().push_back(cb);
        return cb;
    }
    void DetachLogCapture(const std::shared_ptr<spdlog::sinks::callback_sink_mt>& cb)
    {
        auto& sinks = Arcane::Log::Engine()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), cb), sinks.end());
    }

    // A fabricated, never-dereferenced nri::Descriptor* -- same technique
    // ImplNONE's own DummyObject<T>() uses internally (ThirdParty/NRI/
    // Source/NONE/ImplNONE.cpp:8-10). Safe here because Add() only ever
    // stores the pointer; these cases never bury it.
    nri::Descriptor* FakeSrv(std::uintptr_t tag)
    {
        return reinterpret_cast<nri::Descriptor*>(tag);
    }
}

TEST_CASE("BindlessTable: Create refuses a zero capacity", "[bindless]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto table = Arcane::BindlessTable::Create(*device, 0);
    CHECK(table == nullptr);
}

TEST_CASE("BindlessTable: sequential Add calls are assigned dense slots starting at 0", "[bindless]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto table = Arcane::BindlessTable::Create(*device, 8);
    REQUIRE(table != nullptr);

    for (std::uint32_t i = 0; i < 5; ++i)
        CHECK(table->Add(FakeSrv(i + 1)) == i);
}

TEST_CASE("BindlessTable: Add past capacity returns kInvalidSlot and warns exactly once", "[bindless]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto table = Arcane::BindlessTable::Create(*device, 2);
    REQUIRE(table != nullptr);

    CHECK(table->Add(FakeSrv(1)) == 0);
    CHECK(table->Add(FakeSrv(2)) == 1);

    std::string captured;
    int warnCount = 0;
    auto cb = AttachLogCapture(captured, warnCount);

    // Three refused Adds in a row -- only the FIRST may warn.
    CHECK(table->Add(FakeSrv(3)) == Arcane::BindlessTable::kInvalidSlot);
    CHECK(table->Add(FakeSrv(4)) == Arcane::BindlessTable::kInvalidSlot);
    CHECK(table->Add(FakeSrv(5)) == Arcane::BindlessTable::kInvalidSlot);

    DetachLogCapture(cb);

    CHECK(warnCount == 1);
    CHECK(captured.find("BindlessTable") != std::string::npos);
}

TEST_CASE("BindlessTable: Release buries its descriptors via Graveyard::Bury, not direct destruction",
          "[bindless]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto table = Arcane::BindlessTable::Create(*device, 4);
    REQUIRE(table != nullptr);

    // Real descriptors (not fabricated pointers): this case actually reaps
    // the burial and DestroyDescriptor runs for real, so it should prove
    // that succeeds against genuine NRI-created handles.
    const nri::CoreInterface& core = device->Core();
    nri::SamplerDesc samplerDesc{};
    nri::Descriptor* d0 = nullptr;
    nri::Descriptor* d1 = nullptr;
    nri::Descriptor* d2 = nullptr;
    REQUIRE(core.CreateSampler(device->Device(), samplerDesc, d0) == nri::Result::SUCCESS);
    REQUIRE(core.CreateSampler(device->Device(), samplerDesc, d1) == nri::Result::SUCCESS);
    REQUIRE(core.CreateSampler(device->Device(), samplerDesc, d2) == nri::Result::SUCCESS);
    REQUIRE(d0 != nullptr);
    REQUIRE(d1 != nullptr);
    REQUIRE(d2 != nullptr);

    CHECK(table->Add(d0) == 0);
    CHECK(table->Add(d1) == 1);
    CHECK(table->Add(d2) == 2);

    Arcane::Graveyard& graves = device->Graves();
    const std::size_t pendingBefore = graves.Pending();

    table->Release(graves, 7);

    // BURIED, not destroyed: one burial per occupied slot, still pending
    // until Reap() runs them.
    CHECK(graves.Pending() == pendingBefore + 3);

    graves.Reap(7);
    CHECK(graves.Pending() == pendingBefore);

    // Idempotent: the table is now empty, so a second Release buries nothing.
    table->Release(graves, 8);
    CHECK(graves.Pending() == pendingBefore);

    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("BindlessTable: destroyed without Release() warns once and does not crash (safety net, not the path)",
          "[bindless]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto table = Arcane::BindlessTable::Create(*device, 2);
    REQUIRE(table != nullptr);

    // Real descriptors, same reasoning as the Release() case above: the
    // destructor's DeviceWaitIdle + DestroyDescriptor calls should run
    // against genuine handles, not just avoid crashing on a fake one.
    const nri::CoreInterface& core = device->Core();
    nri::SamplerDesc samplerDesc{};
    nri::Descriptor* d0 = nullptr;
    nri::Descriptor* d1 = nullptr;
    REQUIRE(core.CreateSampler(device->Device(), samplerDesc, d0) == nri::Result::SUCCESS);
    REQUIRE(core.CreateSampler(device->Device(), samplerDesc, d1) == nri::Result::SUCCESS);
    REQUIRE(d0 != nullptr);
    REQUIRE(d1 != nullptr);

    CHECK(table->Add(d0) == 0);
    CHECK(table->Add(d1) == 1);

    std::string captured;
    int warnCount = 0;
    auto cb = AttachLogCapture(captured, warnCount);

    // ~BindlessTable() runs here, WITHOUT Release() ever having been
    // called -- the safety-net path. NONE's DeviceWaitIdle/DestroyDescriptor
    // are both no-ops (ImplNONE.cpp), so there is nothing further to observe
    // on this backend beyond "it warned once and did not crash" -- which is
    // exactly the contract this case pins.
    table.reset();

    DetachLogCapture(cb);

    CHECK(warnCount == 1);
    CHECK(captured.find("BindlessTable") != std::string::npos);
    CHECK(captured.find("Release") != std::string::npos);

    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("BindlessTable: Add(nullptr) is refused -- no slot consumed, no warning", "[bindless]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    auto table = Arcane::BindlessTable::Create(*device, 2);
    REQUIRE(table != nullptr);

    std::string captured;
    int warnCount = 0;
    auto cb = AttachLogCapture(captured, warnCount);

    CHECK(table->Add(nullptr) == Arcane::BindlessTable::kInvalidSlot);

    DetachLogCapture(cb);
    CHECK(warnCount == 0);

    // No slot consumed: the table's real capacity is still fully available.
    CHECK(table->Add(FakeSrv(1)) == 0);
    CHECK(table->Add(FakeSrv(2)) == 1);
}
